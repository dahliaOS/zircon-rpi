// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>

#include "examples/media/audio/chan5/chan5.h"

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);

  examples::ChannelNo5 chan5([&loop]() {
    async::PostTask(loop.dispatcher(), [&loop]() { loop.Quit(); });
  });

  loop.Run();
  return 0;
}
