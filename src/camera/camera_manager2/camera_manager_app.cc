// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/camera_manager2/camera_manager_app.h"

#include <fcntl.h>
#include <lib/fdio/directory.h>

#include <string>

#include <ddk/debug.h>
#include <ddk/driver.h>
#include <fbl/auto_call.h>
#include <fbl/function.h>
#include <fbl/unique_fd.h>

#include "src/camera/camera_manager2/camera_manager_impl.h"
#include "src/camera/camera_manager2/stream_impl.h"

namespace camera {

static const char *kCameraDevicePath = "/dev/class/camera";

CameraManagerApp::CameraManagerApp() : context_(sys::ComponentContext::Create()) {
  // Begin monitoring for plug/unplug events for pluggable cameras.
  device_watcher_ = fsl::DeviceWatcher::Create(
      kCameraDevicePath, fbl::BindMember(this, &CameraManagerApp::OnDeviceFound));
  if (device_watcher_ == nullptr) {
    FXL_LOG(ERROR) << " failed to create DeviceWatcher.";
    return;
  }

  zx_status_t status = fdio_service_connect("/svc/fuchsia.sysmem.Allocator",
                                            sysmem_allocator_.NewRequest().TakeChannel().release());

  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Couldn't connect to sysmem service";
    return;
  }

  context_->outgoing()->AddPublicService<fuchsia::camera2::Manager>(
      [this](fidl::InterfaceRequest<fuchsia::camera2::Manager> request) {
        auto client = std::make_unique<CameraManagerImpl>(std::move(request), this);
        UpdateWithCurrentEvents(client.get());
        clients_.push_back(std::move(client));
      });
}

void CameraManagerApp::UpdateWithCurrentEvents(CameraManagerImpl *client) {
  for (auto &device : active_devices_) {
    client->AddCameraAvailableEvent(device->id());
    if (device->muted()) {
      client->AddMuteEvent(device->id());
    }
  }
}

// The dispatcher loop should be shut down when this destructor is called.
// No further messages should be handled after this destructor is called.
CameraManagerApp::~CameraManagerApp() {
  // Stop monitoring plug/unplug events.  We are shutting down and
  // no longer care about devices coming and going.
  device_watcher_ = nullptr;
  // In case we just discovered any new devices:
  inactive_devices_.clear();
  // Shut down each client to the camera manager.
  clients_.clear();
}

void CameraManagerApp::OnDeviceFound(int dir_fd, const std::string &filename) {
  auto device = VideoDeviceClient::Create(dir_fd, filename);
  if (!device) {
    FXL_LOG(ERROR) << "Failed to create device " << filename;
    return;
  }

  device->Startup(
      [this, id = device->id()](zx_status_t status) { OnDeviceStartupComplete(id, status); });

  // Don't notify clients of a device until we know more about it.
  inactive_devices_.push_back(std::move(device));
}

void CameraManagerApp::OnDeviceStartupComplete(int32_t camera_id, zx_status_t status) {
  for (auto iter = inactive_devices_.begin(); iter != inactive_devices_.end(); iter++) {
    if ((*iter)->id() == camera_id) {
      // Now that we found the device, either put it in the active list,
      // or shut it down, depending on the status.
      if (status == ZX_OK) {
        // We put the newly active device in the front of active_devices_,
        // but it doesn't really matter what order the devices are stored in.
        // TODO(garratt): If a device's info marks it as a previously known device,
        // it should be merged with that device instance.  That will allow the device
        // to maintain its id, mute status, and any other properties that must be retained
        // across boot.
        active_devices_.splice(active_devices_.begin(), inactive_devices_, iter);
        // Notify Clients:
        for (auto &client : clients_) {
          client->AddCameraAvailableEvent(camera_id);
        }
      } else {
        inactive_devices_.erase(iter);
      }
      return;
    }
  }
}

VideoDeviceClient *CameraManagerApp::GetActiveDevice(int32_t camera_id) {
  for (auto &device : active_devices_) {
    if (device->id() == camera_id) {
      return device.get();
    }
  }
  return nullptr;
}

void CameraManagerApp::ConnectToStream(
    int32_t camera_id, fuchsia::camera2::StreamConstraints /*constraints*/,
    fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token,
    fidl::InterfaceRequest<fuchsia::camera2::Stream> client_request,
    fuchsia::camera2::Manager::ConnectToStreamCallback callback) {
  // Create a cleanup function, so we can just return if we get an error.
  auto cleanup = fbl::MakeAutoCall([&callback]() {
    FXL_LOG(ERROR) << "Failed to connect to stream";
    ::fuchsia::sysmem::ImageFormat_2 ret;
    callback(ret);
  });
  // 1: Check that the camera exists:
  auto device = GetActiveDevice(camera_id);
  if (!device) {
    return;
  }
  // 2: Check constraints against the configs that the device offers.  If incompatible, fail.
  // 3: Pick a config, stream and image_format_index
  // TODO(garratt): do this properly.
  uint32_t config_index = 0;
  uint32_t stream_type = 0;
  uint32_t image_format_index = 0;

  // 4: Now check if the stream is currently being used.  If it is, we could:
  //     - A) Close the other stream
  //     - B) Have two clients of one stream
  //     - C) Choose another compatible stream
  //     - D) Refuse this request.
  // For now, we will do:
  //       E) Don't even check

  // 5: Allocate the buffer collection.  The constraints from the device must be applied, as well as
  // constraints for all the image formats being offered.  These should be checked at some point by
  // the camera manager.
  // For now, just make up some constraints
  fuchsia::sysmem::BufferCollectionSyncPtr sysmem_collection;
  zx_status_t status =
      sysmem_allocator_->BindSharedCollection(std::move(token), sysmem_collection.NewRequest());
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to connect to BindSharedCollection.";
    return;
  }

  constexpr uint32_t kFakeNumberOfBuffers = 8;
  fuchsia::sysmem::BufferCollectionConstraints buffer_constraints{};
  buffer_constraints.min_buffer_count = kFakeNumberOfBuffers;
  // Used because every constraints need to have a usage.
  buffer_constraints.usage.display = fuchsia::sysmem::videoUsageHwEncoder;
  status = sysmem_collection->SetConstraints(true, buffer_constraints);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to connect to SetConstraints.";
    return;
  }

  // Create a stream instance to handle the stream protocol:
  std::unique_ptr<StreamImpl> stream;
  fidl::InterfaceRequest<fuchsia::camera2::Stream> device_stream;
  status = StreamImpl::Create(&stream, std::move(sysmem_collection), std::move(client_request),
                              &device_stream);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to create stream instance.";
    printf("Error creating stream!\n");
    return;
  }

  // Add stream to list:
  active_streams_.push_back(std::move(stream));

  // 6: Connect stream to device
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
  active_streams_.back()->GetBufferCollection(&buffer_collection);
  device->CreateStream(config_index, stream_type, image_format_index, std::move(buffer_collection),
                       std::move(device_stream));

  // Return the image format that was selected.  Right now we have not selected anything, so we'll
  // let the AutoCall handle things.
}

std::optional<fuchsia::camera2::DeviceInfo> CameraManagerApp::GetCameraInfo(int32_t camera_id) {
  return {};
}

}  // namespace camera
