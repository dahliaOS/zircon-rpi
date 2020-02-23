// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WEAVE_WEAVESTACK_APP_H_
#define SRC_CONNECTIVITY_WEAVE_WEAVESTACK_APP_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <sys/select.h>

#include <memory>
#include <vector>

#include <Weave/Core/WeaveError.h>
#include <src/lib/fsl/tasks/fd_waiter.h>

namespace weavestack {

class App {
 public:
  App() = default;
  ~App();

  WEAVE_ERROR Init();
  zx_status_t Run(zx::time deadline = zx::time::infinite(), bool once = false);
  void Quit();

  async::Loop* loop() { return &loop_; }

 private:
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  zx_status_t WaitForFd(int fd, uint32_t events);
  zx_status_t StartFdWaiters(void);
  void ClearWaiters();
  void FdHandler(zx_status_t status, uint32_t zero);

  struct {
    fd_set read_fds;
    fd_set write_fds;
    fd_set except_fds;
    int num_fds;
  } fds_;

  std::vector<std::unique_ptr<fsl::FDWaiter>> waiters_;
  async::Loop loop_{&kAsyncLoopConfigAttachToCurrentThread};
};

}  // namespace weavestack

#endif  // SRC_CONNECTIVITY_WEAVE_WEAVESTACK_APP_H_
