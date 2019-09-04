// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/camera_manager2/camera_manager_app.h"

#include <fcntl.h>

#include <string>

#include <ddk/debug.h>
#include <ddk/driver.h>
#include <fbl/function.h>
#include <fbl/unique_fd.h>

#include "src/camera/camera_manager2/camera_manager_impl.h"

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

std::optional<fuchsia::camera2::DeviceInfo> CameraManagerApp::GetCameraInfo(int32_t camera_id) {
  return {};
}

}  // namespace camera
