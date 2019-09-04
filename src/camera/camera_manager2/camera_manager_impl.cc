// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/camera_manager2/camera_manager_impl.h"

#include "src/camera/camera_manager2/camera_manager_app.h"


#include <string>

#include <ddk/debug.h>
#include <ddk/driver.h>

namespace camera {

CameraManagerImpl::CameraManagerImpl(::fidl::InterfaceRequest<fuchsia::camera2::Manager> request,
                                     CameraManagerApp *app)
    : binding_(this, std::move(request)), manager_app_(app) {}

void CameraManagerImpl::AddCameraAvailableEvent(int32_t camera_id) {
  AddCameraEvent({CameraEvent::EventType::CameraAvailable, camera_id});
}

void CameraManagerImpl::AddCameraUnavailableEvent(int32_t camera_id) {
  AddCameraEvent({CameraEvent::EventType::CameraUnavailable, camera_id});
}

void CameraManagerImpl::AddMuteEvent(int32_t camera_id) {
  AddCameraEvent({CameraEvent::EventType::Mute, camera_id});
}

void CameraManagerImpl::AddUnmuteEvent(int32_t camera_id) {
  AddCameraEvent({CameraEvent::EventType::Unmute, camera_id});
}

void CameraManagerImpl::AddCameraEvent(CameraEvent event) {
  if (waiting_for_acknowledgement_) {
    events_to_publish.push_back(event);
  } else {
    // waiting_for_acknowledgement_ should never be false when events_to_publish has entries.
    PublishEvent(event);
  }
}

void CameraManagerImpl::PublishEvent(CameraEvent event) {
  auto camera_info = manager_app_->GetCameraInfo(event.camera_id);
  if (!camera_info) {  // Camera dissapeared!
    // go to next message:
    AcknowledgeCameraEvent();
    return;
  }
  bool last_known_camera = true;
  fuchsia::camera2::DeviceInfo cloned_device_info;
  switch (event.type) {
    case CameraEvent::EventType::CameraAvailable:
      // check if this is the last camera notification:
      for (auto &event : events_to_publish) {
        if (event.type == CameraEvent::EventType::CameraAvailable) {
          last_known_camera = false;
        }
      }
      (*camera_info).Clone(&cloned_device_info);
      binding_.events().OnCameraAvailable(event.camera_id, std::move(cloned_device_info),
                                          last_known_camera);
      break;
    case CameraEvent::EventType::CameraUnavailable:
      binding_.events().OnCameraUnavailable(event.camera_id);
      break;
    case CameraEvent::EventType::Mute:
      binding_.events().OnCameraMuteChanged(event.camera_id, true);
      break;
    case CameraEvent::EventType::Unmute:
      binding_.events().OnCameraMuteChanged(event.camera_id, false);
      break;
  }
  waiting_for_acknowledgement_ = true;
}

void CameraManagerImpl::AcknowledgeCameraEvent() {
  if (events_to_publish.empty()) {
    waiting_for_acknowledgement_ = false;
    return;
  }
  auto event = events_to_publish.front();
  events_to_publish.pop_front();
  PublishEvent(event);
}

void CameraManagerImpl::ConnectToStream(
    int32_t camera_id, fuchsia::camera2::StreamConstraints constraints,
    fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token,
    fidl::InterfaceRequest<fuchsia::camera2::Stream> stream, ConnectToStreamCallback callback) {
  // TODO(garratt) implement.
}

}  // namespace camera
