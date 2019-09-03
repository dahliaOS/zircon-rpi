// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/camera_manager2/video_device_client.h"

#include <fcntl.h>
#include <lib/async/cpp/task.h>
#include <lib/fzl/fdio.h>

#include <fbl/unique_fd.h>
#include <src/lib/fxl/logging.h>
#include <src/lib/fxl/strings/string_printf.h>

namespace camera {

std::unique_ptr<VideoDeviceClient> VideoDeviceClient::Create(int dir_fd, const std::string& name) {
  // Open the device node.
  fbl::unique_fd dev_node{openat(dir_fd, name.c_str(), O_RDONLY)};
  if (!dev_node.is_valid()) {
    FXL_LOG(WARNING) << "VideoDeviceClient failed to open device node at \"" << name << "\". ("
                     << strerror(errno) << " : " << errno << ")";
    return nullptr;
  }

  fzl::FdioCaller dev(std::move(dev_node));
  std::unique_ptr<VideoDeviceClient> device(new VideoDeviceClient);
  device->camera_control_.Bind(zx::channel(dev.borrow_channel()));
  return device;
}

void VideoDeviceClient::Startup(StartupCallback callback) {
  // A timeout is also given which will call the callback with an error.
  camera_control_->GetDeviceInfo([this, &callback](fuchsia::camera2::DeviceInfo device_info) {
    device_info_ = std::move(device_info);
    callback(ZX_OK);
  });
}

}  // namespace camera
