// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#include "kernel/semaphore.h"

#include <err.h>
#include <zircon/compiler.h>

#include <kernel/thread_lock.h>

void Semaphore::Post() {
  Guard<spin_lock_t, IrqSave> guard{ThreadLock::Get()};
  count_++;
  waitq_.WakeOne(true, ZX_OK);
}

zx_status_t Semaphore::Wait(const Deadline& deadline) {
  thread_t* current_thread = get_current_thread();

  {
    Guard<spin_lock_t, IrqSave> guard{ThreadLock::Get()};
    while (count_ == 0) {
      current_thread->interruptable = true;
      const zx_status_t ret = waitq_.Block(deadline);
      current_thread->interruptable = false;

      if (ret != ZX_OK && count_ == 0) {
        return ret;
      }
    }
    count_--;
  }

  return ZX_OK;
}
