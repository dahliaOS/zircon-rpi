// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_CAMERA_MANAGER_VIDEO_DEVICE_CLIENT_H_
#define SRC_CAMERA_CAMERA_MANAGER_VIDEO_DEVICE_CLIENT_H_

#include <fuchsia/camera2/cpp/fidl.h>
#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/wait.h>
// #include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fit/function.h>

#include <list>

namespace camera {

// Client class for cameras and other video devices.  This class is intended to
// be used by the CameraManager, not applications.
class VideoDeviceClient {

 public:
  using StartupCallback = ::fit::function<void(zx_status_t status)>;

  // Create a VideoDeviceClient from a folder and filename.  This should be
  // by the CameraManagerImpl when it detects a new or existing device.
  static std::unique_ptr<VideoDeviceClient> Create(int dir_fd, const std::string &name);

  // Load all information to identify the device, as well as available formats.
  // Will call |callback| when everything is loaded.
  void Startup(StartupCallback callback);

  // Gets the device description that is published to the applications.
  const fuchsia::camera2::DeviceInfo& GetDeviceInfo() const { return device_info_; }

  int32_t id() const { return device_id_; }
  bool muted() const { return muted_; }

 private:
  VideoDeviceClient() = default;
  fuchsia::camera2::DeviceInfo device_info_;
  int32_t device_id_;
  bool muted_ = false;
  // The connection to the device driver.
  fuchsia::camera2::hal::ControllerPtr camera_control_;
};

}  // namespace camera

#endif  // SRC_CAMERA_CAMERA_MANAGER_VIDEO_DEVICE_CLIENT_H_
