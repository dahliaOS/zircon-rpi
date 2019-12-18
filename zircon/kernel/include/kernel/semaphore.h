// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_SEMAPHORE_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_SEMAPHORE_H_

#include <stdint.h>
#include <zircon/types.h>

#include <kernel/thread.h>

// A basic counting semaphore. It directly uses the low-level wait queue API.
class Semaphore {
 public:
  explicit Semaphore(uint64_t initial_count = 0) : count_{initial_count} {}

  ~Semaphore() = default;

  Semaphore(const Semaphore&) = delete;
  Semaphore& operator=(const Semaphore&) = delete;

  Semaphore(Semaphore&&) = delete;
  Semaphore& operator=(Semaphore&&) = delete;

  // Increment the counter, possibly releasing one thread.
  void Post();

  // Interruptable wait for the counter to be > 0 or for |deadline| to pass.
  // If the wait was satisfied by Post() the return is ZX_OK and the count is
  // decremented by one.
  // Otherwise the count is not decremented. The return value can be
  // ZX_ERR_TIMED_OUT if the deadline had passed or one of ZX_ERR_INTERNAL_INTR
  // errors if the thread had a signal delivered.
  zx_status_t Wait(const Deadline& deadline);

  // Nested type that implements test access.
  struct Test;

 private:
  uint64_t count_;
  WaitQueue waitq_;
};

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_SEMAPHORE_H_
