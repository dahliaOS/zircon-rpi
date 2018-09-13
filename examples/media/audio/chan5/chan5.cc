// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "examples/media/audio/chan5/chan5.h"

#include <fuchsia/media/cpp/fidl.h>
#include <lib/async-loop/loop.h>
#include <lib/async/cpp/task.h>
#include <poll.h>
#include <src/lib/fxl/logging.h>

#include <iostream>

namespace examples {

ChannelNo5::ChannelNo5(fit::closure quit_callback)
    : quit_callback_(std::move(quit_callback)) {
  // Connect to the audio service and get an AudioRenderer.
  auto startup_context = sys::ComponentContext::Create();

  audio_device_enumerator_ =
      startup_context->svc()->Connect<fuchsia::media::AudioDeviceEnumerator>();

  audio_device_enumerator_.set_error_handler([this](zx_status_t status) {
    std::cerr << "Unexpected error: channel to audio service closed\n";
    Quit();
  });

  zx::channel remote_channel;
  zx_status_t status =
      zx::channel::create(0u, &local_channel_, &remote_channel);
  FXL_CHECK(status == ZX_OK) << "Failed to create channel (" << status << ")";

  std::string device_name = "Aromatic Arias";
  audio_device_enumerator_->AddDeviceByChannel(std::move(remote_channel),
                                               device_name, false);

  std::cout << "Press any key to continue...\n";
  WaitForKeystroke();
}

ChannelNo5::~ChannelNo5() = default;

void ChannelNo5::Quit() {
  audio_device_enumerator_.Unbind();
  quit_callback_();
}

void ChannelNo5::WaitForKeystroke() {
  fd_waiter_.Wait([this](zx_status_t status, uint32_t events) { Quit(); }, 0,
                  POLLIN);
}

}  // namespace examples
