// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbl/algorithm.h>
#include <fbl/auto_call.h>
#include <fbl/string.h>
#include <fbl/string_buffer.h>
#include <fbl/string_piece.h>
#include <fbl/string_printf.h>
#include <fbl/unique_fd.h>
#include <fs-management/mount.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/partition/c/fidl.h>
#include <gpt/gpt.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/fzl/time.h>
#include <lib/zx/channel.h>
#include <lib/zx/process.h>
#include <lib/zx/time.h>
#include <loader-service/loader-service.h>
#include <minfs/minfs.h>
#include <zircon/device/block.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zxcrypt/fdio-volume.h>

#include <utility>

#include "block-device.h"
#include "block-watcher.h"
#include "fs-manager.h"
#include "pkgfs-launcher.h"
#include "zircon/errors.h"

namespace devmgr {
namespace {

constexpr char kPathBlockDeviceRoot[] = "/dev/class/block";

// The number of times Pause() has been called on the block watcher.
static unsigned int watcher_pause_count = 0;
// This mutex is held when pause_count is being modified, and while the
// block watcher is touching a block device.
static std::mutex watcher_callback_mutex;

std::mutex& GetWatcherLock() {
  return watcher_callback_mutex;
}

zx_status_t BlockDeviceCallback(int dirfd, int event, const char* name, void* cookie) {
  if (event == WATCH_EVENT_REMOVE_FILE) {
    fprintf(stderr, "fshost: (%s/%s) removed\n", kPathBlockDeviceRoot, name);
    return ZX_OK;
  }
  if (event != WATCH_EVENT_ADD_FILE) {
    return ZX_OK;
  }
  fbl::unique_fd device_fd(openat(dirfd, name, O_RDWR));
  if (!device_fd) {
    return ZX_OK;
  }

  // Lock the block watcher, so any pause operations wait until after we're done.
  auto lock = std::lock_guard<std::mutex>{GetWatcherLock()};
  if (watcher_pause_count != 0) {
    return ZX_OK;
  }

  fprintf(stderr, "fshost: (%s/%s) added, will attempt to bind.\n", kPathBlockDeviceRoot, name);
  auto mounter = static_cast<FilesystemMounter*>(cookie);
  BlockDevice device(mounter, std::move(device_fd));
  zx_status_t rc = device.Add();
  if (rc != ZX_OK) {
    // This callback has to return ZX_OK for resiliency reasons, or we'll
    // stop getting subsequent callbacks, but we should log loudly that we
    // tried to do something and that failed.
    fprintf(stderr, "fshost: (%s/%s) failed: %s\n", kPathBlockDeviceRoot, name,
            zx_status_get_string(rc));
  }
  return ZX_OK;
}

}  // namespace

void BlockDeviceWatcher(std::unique_ptr<FsManager> fshost, BlockWatcherOptions options) {
  FilesystemMounter mounter(std::move(fshost), options);

  fbl::unique_fd dirfd(open(kPathBlockDeviceRoot, O_DIRECTORY | O_RDONLY));
  if (dirfd) {
    fdio_watch_directory(dirfd.get(), BlockDeviceCallback, ZX_TIME_INFINITE, &mounter);
  }
}

fbl::RefPtr<fs::Service> BlockWatcherServer::Create(devmgr::FsManager* fs_manager,
                                                           async_dispatcher* dispatcher) {
  return fbl::MakeRefCounted<fs::Service>([dispatcher, fs_manager](zx::channel chan) mutable {
    zx::event event;
    zx_status_t status = fs_manager->event()->duplicate(ZX_RIGHT_SAME_RIGHTS, &event);
    if (status != ZX_OK) {
      fprintf(stderr, "fshost: failed to duplicate event handle for admin service: %s\n",
              zx_status_get_string(status));
      return status;
    }

    status = fidl::Bind(dispatcher, std::move(chan), std::make_unique<BlockWatcherServer>());
    if (status != ZX_OK) {
      fprintf(stderr, "fshost: failed to bind admin service: %s\n", zx_status_get_string(status));
      return status;
    }
    return ZX_OK;
  });
}

void BlockWatcherServer::Pause(PauseCompleter::Sync completer) {
  auto lock = std::lock_guard<std::mutex>{GetWatcherLock()};
  if (watcher_pause_count == std::numeric_limits<unsigned int>::max()) {
    completer.Reply(ZX_ERR_BAD_STATE);
    return;
  } else {
    watcher_pause_count++;
    completer.Reply(ZX_OK);
  }
}

void BlockWatcherServer::Resume(ResumeCompleter::Sync completer) {
  auto lock = std::lock_guard<std::mutex>{GetWatcherLock()};
  if (watcher_pause_count == 0) {
    completer.Reply(ZX_ERR_BAD_STATE);
    return;
  } else {
    watcher_pause_count--;
    completer.Reply(ZX_OK);
  }
}
}  // namespace devmgr
