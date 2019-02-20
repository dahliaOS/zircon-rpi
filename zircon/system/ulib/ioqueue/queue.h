// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <ioqueue/queue.h>
#include <zircon/types.h>

#include "scheduler.h"
#include "worker.h"

// Dummy class for easy casting between IoQueue and Queue.
class IoQueue {
public:
    IoQueue() = default;
    ~IoQueue() = default;
};

namespace ioqueue {

class Queue : public IoQueue {
public:
    // Client API - synchronous calls.
    // -------------------------------
    Queue(const IoQueueCallbacks* cb);
    ~Queue();

    // Open a stream of ops.
    zx_status_t OpenStream(uint32_t priority, uint32_t id);
    // Close a stream. Blocks until all ops have completed.
    zx_status_t CloseStream(uint32_t id);

    // Return pointer to scheduler.
    Scheduler* GetScheduler() { return &sched_; }

    // Begin service. Worker threads are created, and ops are acquired and issued.
    zx_status_t Serve(uint32_t num_workers);
    // Close all streams and wait for completion.
    void Shutdown();

    // Client API - asynchronous calls.
    // --------------------------------
    void AsyncCompleteOp(Op* op) { sched_.CompleteOp(op, true); }

    // API invoked by worker threads.
    // --------------------------------
    // A worker thread is exiting.
    void WorkerExited(uint32_t id);
    // Attempt to get a turn to call acquire.
    zx_status_t GetAcquireSlot();
    // Make acquire available to other workers.
    void ReleaseAcquireSlot();
    // Read in ops. Acquire slot must be held.
    zx_status_t AcquireOps(Op** op_list, size_t* op_count, bool wait);
    zx_status_t IssueOp(Op* op);
    void ReleaseOp(Op* op);

private:
    Scheduler sched_{};

    fbl::Mutex lock_;
    // The below entries are protected by lock_.
    bool shutdown_ = true;          // Queue has been shut down.
    uint32_t num_workers_ = 0;      // Number of worker threads.
    uint32_t active_workers_ = 0;   // Number of active workers.
    fbl::ConditionVariable event_workers_exited_;    // All workers have exited.
    uint32_t acquire_workers_ = 0;  // Number of worker threads in acquire.
    const IoQueueCallbacks* ops_ = nullptr;
    Worker worker[kIoQueueMaxWorkers]; // Array of worker objects.
};

} // namespace
