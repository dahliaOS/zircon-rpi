// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdio.h>

#include "op.h"
#include "queue.h"
#include "scheduler.h"
#include "worker.h"

namespace ioqueue {

zx_status_t Worker::Launch(Queue* q, uint32_t id, uint32_t priority) {
    assert(!thread_running_);
    q_ = q;
    id_ = id;
    priority_ = priority;
    if (thrd_create(&thread_, ThreadEntry, this) != ZX_OK) {
        fprintf(stderr, "Worker failed to create thread\n");
        // Shutdown required. TODO, signal fatal error.
        return ZX_ERR_NO_MEMORY;
    }
    thread_running_ = true;
    return ZX_OK;
}

void Worker::Join() {
    assert(thread_running_);
    thrd_join(thread_, nullptr);
    thread_running_ = false;
}

int Worker::ThreadEntry(void* arg) {
    Worker* w = static_cast<Worker*>(arg);
    w->ThreadMain();
    return 0;
}

void Worker::ThreadMain() {
    // printf("worker %u: started\n", id_);
    WorkerLoop();
    q_->WorkerExited(id_);
}

#define NUM_COMPLETED_OPS 10

void Worker::WorkerLoop() {
    // printf("%s:%u\n", __FUNCTION__, __LINE__);

    Scheduler* sched = q_->GetScheduler();
    do {
        zx_status_t status;
        size_t num_ready = 0;
        // Drain completed ops.
        for ( ; ; ) {
            size_t op_count = 0;
            Op* op_list[NUM_COMPLETED_OPS];
            status = sched->GetCompletedOps(op_list, NUM_COMPLETED_OPS, &op_count);
            if ((status != ZX_OK) || (op_count == 0)) {
                break;
            }
            for (size_t i = 0; i < op_count; i++) {
                q_->ReleaseOp(op_list[i]);
            }
        }
         // Read new ops.
        if (!cancelled_) {
            status = AcquireOps(true, &num_ready);
            if (status == ZX_ERR_CANCELED) {
                // Cancel received.
                //      drain the queue and exit.
                assert(cancelled_);
            } else if (status != ZX_OK) {
                // Todo: handle better
                assert(false);
            }
        }
        // Issue ready ops.
        for ( ; ; ) {
            // Acquire an issue slot.
            Op* op = nullptr;
            status = sched->GetNextOp(true, &op);
            if (status != ZX_OK) {
                assert((status == ZX_ERR_SHOULD_WAIT) || // No issue slots available.
                       (status == ZX_ERR_UNAVAILABLE));  // No ops available.
                break;
            }
            // Issue slot acquired and op available. Execute it.
            status = q_->IssueOp(op);
            if (status == ZX_ERR_ASYNC) {
                continue;   // Op will be completed asynchronously.
            }
            // Op completed or failed synchronously. Release.
            sched->CompleteOp(op, status);
            q_->ReleaseOp(op);
            op = nullptr; // Op freed in ops->release().
        }
    } while (!cancelled_);
}

zx_status_t Worker::AcquireOps(bool wait, size_t* out_num_ready) {
    const size_t op_list_length = 32;
    Op* op_list[op_list_length];
    zx_status_t status;
    size_t op_count = op_list_length;
    do {
        op_count = 32;
        status = q_->AcquireOps(op_list, &op_count, wait);
        if (status == ZX_ERR_CANCELED) {
            cancelled_ = true;
        }
        if (status != ZX_OK) {
            return status;
        }
    } while (op_count == 0);
    Scheduler* sched = q_->GetScheduler();
    sched->InsertOps(op_list, op_count, out_num_ready);
    for (uint32_t i = 0; i < op_count; i++) {
        // Non-null ops encountered errors, release them.
        if (op_list[i] != NULL) {
            q_->ReleaseOp(op_list[i]);
        }
    }
    return ZX_OK;
}

} // namespace
