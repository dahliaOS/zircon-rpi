// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/weave/weavestack/app.h"

#include <lib/async/cpp/task.h>
#include <lib/syslog/cpp/logger.h>
#include <poll.h>
#include <zircon/types.h>

#include <Weave/DeviceLayer/PlatformManager.h>

namespace weavestack {

namespace {
using nl::Weave::DeviceLayer::PlatformMgr;
using nl::Weave::DeviceLayer::PlatformMgrImpl;
}  // namespace

App::~App() { Quit(); }

WEAVE_ERROR App::Init() {
  syslog::InitLogger({"weavestack"});

  WEAVE_ERROR err = PlatformMgr().InitWeaveStack();
  if (err != WEAVE_NO_ERROR) {
    FX_LOGS(ERROR) << "InitWeaveStack() failed: " << nl::ErrorStr(err);
  }

  return err;
}

zx_status_t App::WaitForFd(int fd, uint32_t events) {
  FX_LOGS(INFO) << "waiting for events = " << events << " from fd = " << fd << "...";

  auto waiter = std::make_unique<fsl::FDWaiter>(loop_.dispatcher());
  bool waited = waiter->Wait([this](zx_status_t status, uint32_t zero) { FdHandler(status, zero); },
                             fd, events);
  if (!waited) {
    FX_LOGS(ERROR) << "failed to wait for events on fd = " << fd;
  }

  waiters_.push_back(std::move(waiter));
  return ZX_OK;
}

// TODO(fxb/47096): tracks the integration test.
zx_status_t App::StartFdWaiters(void) {
  FX_LOGS(INFO) << "starting new fd waiters for system and inet layers...";

  ClearWaiters();

  struct timeval sleep_time;
  memset(&sleep_time, 0, sizeof(sleep_time));

  PlatformMgrImpl().GetSystemLayer().PrepareSelect(fds_.num_fds, &fds_.read_fds, &fds_.write_fds,
                                                   &fds_.except_fds, sleep_time);
  PlatformMgrImpl().GetInetLayer().PrepareSelect(fds_.num_fds, &fds_.read_fds, &fds_.write_fds,
                                                 &fds_.except_fds, sleep_time);

  for (auto fd = 0; fd < fds_.num_fds; ++fd) {
    uint32_t events = 0;
    if (FD_ISSET(fd, &fds_.read_fds)) {
      events |= POLLIN;
    }
    if (FD_ISSET(fd, &fds_.write_fds)) {
      events |= POLLOUT;
    }
    if (FD_ISSET(fd, &fds_.except_fds)) {
      events |= POLLERR;
    }

    if (events == 0) {
      continue;
    }

    zx_status_t status = WaitForFd(fd, events);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "error waiting for fd " << fd << ": " << status;
      return status;
    }
  }

  return ZX_OK;
}

void App::ClearWaiters() {
  waiters_.clear();
  FD_ZERO(&fds_.read_fds);
  FD_ZERO(&fds_.write_fds);
  FD_ZERO(&fds_.except_fds);
  fds_.num_fds = 0;
}

void App::FdHandler(zx_status_t status, uint32_t zero) {
  if (status == ZX_ERR_CANCELED) {
    FX_VLOGS(1) << "waiter cancelled, doing nothing";
    return;
  }

  struct timeval sleep_time;
  memset(&sleep_time, 0, sizeof(sleep_time));

  // We already know at least one of the fds have an event, but
  // HandleSelectResult expects the result from select.
  //
  // TODO(ghanan): Can we get the effect result of select without calling it?
  int res = select(fds_.num_fds, &fds_.read_fds, &fds_.write_fds, &fds_.except_fds, &sleep_time);
  if (res < 0) {
    FX_LOGS(ERROR) << "failed to select on fds: " << strerror(errno);
    loop_.Shutdown();
    return;
  }

  FX_VLOGS(1) << "handling system layer results...";
  PlatformMgrImpl().GetSystemLayer().HandleSelectResult(res, &fds_.read_fds, &fds_.write_fds,
                                                        &fds_.except_fds);

  FX_VLOGS(1) << "handling inet layer results...";
  PlatformMgrImpl().GetInetLayer().HandleSelectResult(res, &fds_.read_fds, &fds_.write_fds,
                                                      &fds_.except_fds);

  // Wait for the next set of events.
  status = StartFdWaiters();
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "failed to wait for next packet: " << status;
    loop_.Shutdown();
  }
}

zx_status_t App::Run(zx::time deadline, bool once) {
  async::PostTask(loop_.dispatcher(), [this]() {
    zx_status_t status = StartFdWaiters();
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "failed to wait for first packet: " << status;
      loop_.Shutdown();
    }
  });

  FX_LOGS(INFO) << "running the eventloop...";

  zx_status_t status = loop_.Run(deadline, once);
  FX_LOGS(WARNING) << "eventloop ended with status = " << status;
  return status;
}

void App::Quit() {
  loop_.Quit();
  loop_.JoinThreads();
  ClearWaiters();
}

}  // namespace weavestack
