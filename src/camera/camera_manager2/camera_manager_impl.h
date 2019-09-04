// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_CAMERA_MANAGER2_CAMERA_MANAGER_IMPL_H_
#define SRC_CAMERA_CAMERA_MANAGER2_CAMERA_MANAGER_IMPL_H_

#include <fuchsia/camera2/cpp/fidl.h>
#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fsl/io/device_watcher.h>

#include <deque>

#include <ddk/debug.h>
#include <ddk/driver.h>
#include <src/camera/camera_manager2/video_device_client.h>

namespace camera {
// Implements camera2::Manager FIDL service.  Keeps track of the cameras and
// other video input devices that are plugged in, making that information
// available to applications.  Also, keeps track of the connections to a
// device, ensuring that applications do not open more connections than the
// device can support.
class CameraManagerApp;
class CameraManagerImpl : public fuchsia::camera2::Manager {
 public:
  // In addition to shuting down the camera::Manager service, this destructor
  // will attempt to cancel all video streams, even if they are connected
  // directly from the device driver to the application.
  ~CameraManagerImpl() override{};

  // This initialization is passed the async::Loop because it will be stepping
  // the loop forward until all the devices are enumerated. |loop| should be
  // the async loop associated with the default dispatcher.
  // This constructor will not return until all existing camera devices have
  // been enumerated and set up.
  CameraManagerImpl(fidl::InterfaceRequest<fuchsia::camera2::Manager> request,
                    CameraManagerApp *app);

  // Connect to a camera stream:
  // |camera_id| Refers to a specific camera_id from a CameraInfo that has been
  // advertised by OnCameraAvailable.
  // |constraints| contains a set of constraints on the requested stream.  The Camera
  // Manager will attempt to find a stream that meets the constraints.  If multiple
  // streams match, one of the matching streams will be connected.
  // |token| refers to a Sysmem buffer allocation that will be used to pass images using
  // the Stream protocol.  The Camera Manager will apply a BufferCollectionContraints
  // related to the image format(s), so the client does not need to apply any
  // ImageFormatConstraints.
  // Sync is assumed to have been called on |token| before it is passed to
  // ConnectToStream.
  // Since |constraints| may not dictate a specific format, the initial format of images
  // on the stream is indicated on the response.
  // The connection is considered to be successful once a response has been given, unless
  // |stream| is closed.
  void ConnectToStream(int32_t camera_id, fuchsia::camera2::StreamConstraints constraints,
                       fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token,
                       fidl::InterfaceRequest<fuchsia::camera2::Stream> stream,
                       ConnectToStreamCallback callback) override;

  // Provides flow control.  The client must acknowledge every event before
  // more events can be sent.
  void AcknowledgeCameraEvent() override;

  // Called by the CameraCore when events happen:
  void AddCameraAvailableEvent(int32_t camera_id);
  void AddCameraUnavailableEvent(int32_t camera_id);
  void AddMuteEvent(int32_t camera_id);
  void AddUnmuteEvent(int32_t camera_id);

 private:
  struct CameraEvent {
    enum EventType { CameraAvailable, CameraUnavailable, Mute, Unmute };
    EventType type;
    int64_t camera_id;
  };

  std::deque<CameraEvent> events_to_publish;

  // Add event to queue of events that will be sent to the client.
  void AddCameraEvent(CameraEvent event);
  void PublishEvent(CameraEvent event);

  fidl::Binding<fuchsia::camera2::Manager> binding_;
  CameraManagerApp *manager_app_;
  bool waiting_for_acknowledgement_ = false;
};

}  // namespace camera

#endif  // SRC_CAMERA_CAMERA_MANAGER2_CAMERA_MANAGER_IMPL_H_
