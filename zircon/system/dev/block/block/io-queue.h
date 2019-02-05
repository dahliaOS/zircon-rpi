// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <zircon/types.h>

#include "io-op.h"
#include "io-scheduler.h"
#include "io-worker.h"

#define IO_QUEUE_MAX_WORKERS        8

namespace ioqueue {

struct QueueOps {
    // Returns true if second op can be reordered ahead of the first one.
    // bool (*can_reorder)(struct io_queue* q, io_op_t* first, io_op_t* second);
    // Get ops from source.
    zx_status_t (*acquire)(void* context, io_op_t** op_list, size_t* op_count, bool wait);
    // Executes the op. May not be called if dependencies have failed.
    zx_status_t (*issue)(void* context, io_op_t* op);
    // An op has completed. Called once for every scheduled op. Queue maintains no references
    // to |op| after this call and it is safe to delete or reuse.
    void (*release)(void* context, io_op_t* op);
    // Called during shutdown to interrupt blocked acquire callback.
    // Acquire calls following this should return ZX_ERR_CANCELED.
    void (*cancel_acquire)(void* context);
    // A fatal error has occurred. Queue should be shut down.
    void (*fatal)(void* context);
    // User-provided context structure returned in the above callbacks.
    void* context;
};

class Queue {
public:
    // Client API - synchronous calls.
    // -------------------------------
    Queue(const QueueOps* ops);
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
    void AsyncCompleteOp(io_op_t* op, zx_status_t result) { sched_.CompleteOp(op, result); }

    // API invoked by worker threads.
    // ------------------------------
    zx_status_t WorkerAcquireLoop();
    zx_status_t WorkerIssueLoop();
    void WorkerExited(uint32_t id);
    zx_status_t GetAcquireSlot();
    void ReleaseAcquireSlot();
    zx_status_t AcquireOps(io_op_t** op_list, size_t* op_count, bool wait);
    zx_status_t IssueOp(io_op_t* op);
    void ReleaseOp(io_op_t* op);

private:
    Scheduler sched_{};

    fbl::Mutex lock_;
    bool shutdown_ = true;          // Queue has been shut down.
    uint32_t num_workers_ = 0;      // Number of worker threads.
    uint32_t active_workers_ = 0;   // Number of active workers.
    cnd_t event_workers_exited_;    // All workers have exited.
    uint32_t acquire_workers_ = 0;  // Number of worker threads in acquire.
    const QueueOps* ops_ = nullptr;
    Worker worker[IO_QUEUE_MAX_WORKERS]; // Array of worker objects.
};

} // namespace
