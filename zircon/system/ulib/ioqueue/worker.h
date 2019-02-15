// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>
#include <threads.h>

#include <zircon/types.h>

namespace ioqueue {

class Queue;

class Worker {
public:
    // Create a worker thread.
    zx_status_t Launch(Queue* q, uint32_t id, uint32_t priority);
    // Join an attached worker thread. Thread must be sent exit signal before joining.
    void Join();

private:
    static int ThreadEntry(void* arg);
    void ThreadMain();
    void WorkerLoop();
    zx_status_t AcquireLoop();
    zx_status_t IssueLoop();
    zx_status_t AcquireOps(bool wait, size_t* out_num_ready);

    Queue* q_ = nullptr;
    bool cancelled_ = false;
    bool thread_running_ = false;
    uint32_t id_;
    uint32_t priority_;
    thrd_t thread_;
};

} // namespace