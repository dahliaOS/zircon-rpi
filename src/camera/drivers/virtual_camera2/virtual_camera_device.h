// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_VIRTUAL_CAMERA2_VIRTUAL_CAMERA_DEVICE_H_
#define SRC_CAMERA_DRIVERS_VIRTUAL_CAMERA2_VIRTUAL_CAMERA_DEVICE_H_

#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <fuchsia/hardware/camera/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/default.h>

#include <ddk/protocol/test.h>
#include <ddktl/device-internal.h>
#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>

namespace virtual_camera {

class VirtualCameraDevice {
 public:
  VirtualCameraDevice();
  ~VirtualCameraDevice();

  // DDK device implementation
  void Unbind();

  void Release();

  zx_status_t Message(fidl_msg_t* msg, fidl_txn_t* txn);

  zx_status_t Bind(zx_device_t* device);

  zx_device_t* dev_node() const { return dev_node_; }

 private:
  // Device FIDL implementation

  // Get a list of all available configurations which the camera driver supports.
  void GetConfigs(GetConfigsCallback callback);

  // Set a particular configuration and create the requested stream.
  // |config_index| : Configuration index from the vector which needs to be applied.
  // |stream_type| : Stream types (one of more of |CameraStreamTypes|)
  // |buffer_collection| : Buffer collections for the stream.
  // |stream| : Stream channel for the stream requested
  // |image_format_index| : Image format index which needs to be set up upon creation.
  // If there is already an active configuration which is different than the one
  // which is requested to be set, then the HAL will be closing all existing streams
  // and honor this new setup call.
  // If the new stream requested is already part of the existing running configuration
  // the HAL will just be creating this new stream while the other stream still exists as is.
  void CreateStream(uint32_t config_index, uint32_t stream_type, uint32_t image_format_index, ::fuchsia::sysmem::BufferCollectionInfo buffer_collection, ::fidl::InterfaceRequest<::fuchsia::camera2::Stream> stream);

  // Enable/Disable Streaming
  void EnableStreaming();

  void DisableStreaming();

  void GetDeviceInfo(GetDeviceInfoCallback callback);

  static const fuchsia_hardware_camera_Device_ops_t CAMERA_FIDL_THUNKS;

  zx_device_t* dev_node_ = nullptr;

  static std::unique_ptr<async::Loop> fidl_dispatch_loop_;
};

}  // namespace virtual_camera

#endif  // SRC_CAMERA_DRIVERS_VIRTUAL_CAMERA2_VIRTUAL_CAMERA_DEVICE_H_
