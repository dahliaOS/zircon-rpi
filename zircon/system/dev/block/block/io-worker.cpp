// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdio.h>

#include "io-op.h"
#include "io-queue.h"
#include "io-scheduler.h"
#include "io-worker.h"

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

    zx_status_t status;
    size_t num_ready;
    Scheduler* sched = q_->GetScheduler();
    do {
        // Drain completed ops.
        for ( ; ; ) {
            size_t op_count = 0;
            io_op_t* op_list[NUM_COMPLETED_OPS];
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
            io_op_t* op = nullptr;
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

// zx_status_t Worker::AcquireLoop() {
//     printf("%s:%u\n", __FUNCTION__, __LINE__);
//     zx_status_t status = q_->GetAcquireSlot();
//     if (status != ZX_OK) {
//         // Acquire slots are full, not an error.
//         printf("%s:%u\n", __FUNCTION__, __LINE__);
//         return ZX_OK;
//     }

//     Scheduler* sched = q_->GetScheduler();
//     uint32_t num_ready = sched->NumReadyOps();
//     for ( ; status == ZX_OK; ) {
//         if (num_ready >= SCHED_OPS_HIWAT) {
//             break;  // Queue is full, don't read.
//         }
//         bool wait = true;
//         if (num_ready > SCHED_OPS_LOWAT) {
//             wait = false;   // Non-blocking read.
//         }
//         status = AcquireOps(wait, &num_ready);
//     }

//     q_->ReleaseAcquireSlot();
//     printf("%s:%u\n", __FUNCTION__, __LINE__);
//     return status;
// }

zx_status_t Worker::AcquireOps(bool wait, size_t* out_num_ready) {
    io_op_t* op_list[32];
    zx_status_t status;
    size_t op_count;
    do {
        op_count = (sizeof(op_list) / sizeof(io_op_t*));
        status = q_->AcquireOps(op_list, &op_count, wait);
        if (status == ZX_ERR_CANCELED) {
            cancelled_ = true;
        }
        if (status != ZX_OK) {
            return status;
        }
    } while (op_count == 0);
    Scheduler* sched = q_->GetScheduler();
    if ((status = sched->InsertOps(op_list, op_count, out_num_ready)) != ZX_OK) {
        for (uint32_t i = 0; i < op_count; i++) {
            // Non-null ops encountered errors, release them.
            if (op_list[i] != NULL) {
                q_->ReleaseOp(op_list[i]);
            }
        }
    }
    return ZX_OK;
}

// zx_status_t Worker::IssueLoop() {
//     printf("%s:%u\n", __FUNCTION__, __LINE__);
//     Scheduler* sched = q_->GetScheduler();
//     bool wait = true /*cancelled_*/;
//     for ( ; ; ) {
//         // Acquire an issue slot.
//         io_op_t* op = nullptr;
//         zx_status_t status = sched->GetNextOp(wait, &op);
//         if (status != ZX_OK) {
//             assert((status == ZX_ERR_SHOULD_WAIT) || // No issue slots available.
//                    (status == ZX_ERR_UNAVAILABLE));  // No ops available.
//             return status;
//         }
//         // Issue slot acquired and op available. Execute it.
//         status = q_->OpIssue(op);
//         if (status == ZX_ERR_ASYNC) {
//             continue;   // Op will be completed asynchronously.
//         }
//         // Op completed or failed synchronously. Release.
//         sched->CompleteOp(op, status);
//         q_->OpRelease(op);
//         op = nullptr; // Op freed in ops->release().
//     }
// }

} // namespace
