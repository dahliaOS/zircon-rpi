// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EXAMPLES_MEDIA_AUDIO_CHAN5_CHAN5_H_
#define EXAMPLES_MEDIA_AUDIO_CHAN5_CHAN5_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/fit/function.h>
#include <src/lib/fxl/macros.h>

#include "lib/fsl/tasks/fd_waiter.h"
#include "lib/sys/cpp/component_context.h"

namespace examples {

class ChannelNo5 {
 public:
  ChannelNo5(fit::closure quit_callback);

  ~ChannelNo5();

 private:
  // Quits the app.
  void Quit();

  // Calls |HandleKeystroke| on the message loop when console input is ready.
  void WaitForKeystroke();

  // Handles a keystroke, possibly calling |WaitForKeystroke| to wait for the
  // next one.
  void HandleKeystroke(int c);

  fit::closure quit_callback_;
  fsl::FDWaiter fd_waiter_;
  fuchsia::media::AudioDeviceEnumeratorPtr audio_device_enumerator_;
  zx::channel local_channel_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ChannelNo5);
};

}  // namespace examples

#endif  // EXAMPLES_MEDIA_AUDIO_CHAN5_CHAN5_H_
