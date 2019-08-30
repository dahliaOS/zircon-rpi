// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_CAMERA_MANAGER_CAMERA_MANAGER_IMPL_H_
#define SRC_CAMERA_CAMERA_MANAGER_CAMERA_MANAGER_IMPL_H_

#include <fuchsia/camera/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fsl/io/device_watcher.h>

#include <list>

#include <ddk/debug.h>
#include <ddk/driver.h>
#include <src/camera/camera_manager/video_device_client.h>

namespace camera {
// Keeps track of the cameras and
// other video input devices that are plugged in, making that information
// available to applications.  Also, keeps track of the connections to a
// device, ensuring that applications do not open more connections than the
// device can support.
class CameraManagerApp : public fuchsia::camera::Manager {
 public:
  // In addition to shutting down the camera::Manager service, this destructor
  // will cancel all video streams, and close all client connections.
  ~CameraManagerApp() override;

  CameraManagerApp();

  std::optional<CameraInfo> GetCameraInfo(int32_t camera_id);

 private:
  // Called when a device is enumerated, or when this class starts, and
  // discovers all the current devices in the system.
  void OnDeviceFound(int dir_fd, const std::string& filename);

  // Called by the device once it finishes initializing.
  void OnDeviceStartupComplete(uint64_t camera_id, zx_status_t status);

  std::list<std::unique_ptr<VideoDeviceClient>> active_devices_;
  // List of not-yet-activated cameras, waiting to get information from
  // the driver.
  std::list<std::unique_ptr<VideoDeviceClient>> inactive_devices_;

  std::unique_ptr<fsl::DeviceWatcher> device_watcher_;

  std::unique_ptr<component::StartupContext> context_;
  fidl::BindingSet<Manager> bindings_;
};

}  // namespace camera

#endif  // SRC_CAMERA_CAMERA_MANAGER_CAMERA_MANAGER_IMPL_H_
