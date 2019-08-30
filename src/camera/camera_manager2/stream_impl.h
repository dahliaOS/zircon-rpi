// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_CAMERA_MANAGER2_STREAM_IMPL_H_
#define SRC_CAMERA_CAMERA_MANAGER2_STREAM_IMPL_H_

#include <fuchsia/camera2/cpp/fidl.h>
#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fsl/io/device_watcher.h>

#include <deque>

#include <ddk/debug.h>
#include <ddk/driver.h>

namespace camera {
// Implements camera2::Stream FIDL service.  Keeps track of the cameras and
// other video input devices that are plugged in, making that information
// available to applications.  Also, keeps track of the connections to a
// device, ensuring that applications do not open more connections than the
// device can support.
class StreamImpl : public fuchsia::camera2::Stream {
 public:
  // In addition to shuting down the camera::Stream service, this destructor
  // will attempt to cancel all video streams, even if they are connected
  // directly from the device driver to the application.
  ~StreamImpl() override = default;

  static zx_status_t Create(std::unique_ptr<StreamImpl> *stream,
                            fuchsia::sysmem::BufferCollectionSyncPtr sysmem_collection,
                            fidl::InterfaceRequest<fuchsia::camera2::Stream> client_request,
                            fidl::InterfaceRequest<fuchsia::camera2::Stream> *device_stream_out) {
    zx_status_t allocation_status = ZX_OK;
    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info{};
    zx_status_t status =
        sysmem_collection->WaitForBuffersAllocated(&allocation_status, &buffer_collection_info);
    if (status != ZX_OK) {
      return status;
    }
    if (allocation_status != ZX_OK) {
      return allocation_status;
    }
    *stream = std::make_unique<StreamImpl>(std::move(client_request), device_stream_out);
    (*stream)->buffer_collection_info_ = std::move(buffer_collection_info);
    (*stream)->sysmem_collection_ = std::move(sysmem_collection);
    return ZX_OK;
  }

  // This initialization is passed the async::Loop because it will be stepping
  // the loop forward until all the devices are enumerated. |loop| should be
  // the async loop associated with the default dispatcher.
  // This constructor will not return until all existing camera devices have
  // been enumerated and set up.
  StreamImpl(fidl::InterfaceRequest<fuchsia::camera2::Stream> request,
             fidl::InterfaceRequest<fuchsia::camera2::Stream> *device_stream_out)
      : binding_(this, std::move(request)) {
    *device_stream_out = std::move(stream_.NewRequest());
  }

  // Control Operations
  // Starts the streaming of frames.
  void Start() override { stream_->Start(); }

  // Stops the streaming of frames.
  void Stop() override { stream_->Stop(); }

  // Unlocks the specified frame, allowing the driver to reuse the memory.
  void ReleaseFrame(uint32_t buffer_id) override { stream_->ReleaseFrame(buffer_id); }

  // Provides flow control for receiving frame errors. See OnFrameAvailable comment.
  void AcknowledgeFrameError() override { stream_->AcknowledgeFrameError(); }

  // Data operations
  // This is used by clients to provide inputs for region of interest
  // selection.
  // Inputs are the x & y coordinates for the new bounding box.
  // For streams which do not support smart framing, this would
  // return an error.
  void SetRegionOfInterest(float x_min, float y_min, float x_max, float y_max,
                           SetRegionOfInterestCallback callback) override {
    callback(ZX_ERR_NOT_SUPPORTED);
  }

  // Change the image format of the stream. This is called when clients want
  // to dynamically change the resolution of the stream while the streaming is
  // is going on.
  void SetImageFormat(uint32_t image_format_index, SetImageFormatCallback callback) override {
    callback(ZX_ERR_NOT_SUPPORTED);
  }

  void GetImageFormats(GetImageFormatsCallback callback) override {
    std::vector<::fuchsia::sysmem::ImageFormat_2> ret;
    callback(ret);
  };

  void GetBufferCollection(fuchsia::sysmem::BufferCollectionInfo_2 *buffer_collection) {}

 private:
  fuchsia::sysmem::BufferCollectionSyncPtr sysmem_collection_;
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info_;
  fidl::Binding<fuchsia::camera2::Stream> binding_;
  fuchsia::camera2::StreamPtr stream_;
};

}  // namespace camera

#endif  // SRC_CAMERA_CAMERA_MANAGER2_STREAM_IMPL_H_
