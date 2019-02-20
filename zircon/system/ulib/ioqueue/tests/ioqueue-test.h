// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fbl/condition_variable.h>
#include <ioqueue/queue.h>
#include <zircon/listnode.h>

// Wrapper around IoOp, the basic work unit of IoQueue.
struct TestOp {
    IoOp op;
    list_node_t node;
    uint32_t id;
    bool enqueued;
    bool issued;
    bool released;
};

class IoQueueTest {
public:
    // Number of threads to use inside IO Queue.
    IoQueueTest(uint32_t num_workers);
    // Associate with IoQueue.
    void SetQueue(IoQueue* q) { q_ = q; }
    // Return IoQueue.
    IoQueue* GetQueue() { return q_; }

    // Add a test op to the op source from which IO Queue will acquire ops.
    void Enqueue(TestOp* top);
    // Wait for all currently enqueued ops to be acquired by IO Queue workers.
    void WaitForAcquired();
    // Wait for all currently enqueued ops to be released by IO Queue workers.
    void WaitForReleased();
    // Mark the op source as closed.
    void CloseInput();

    // Return the number of ops enqueued, issued, and released.
    void GetCounts(uint32_t counts[3]);
    uint32_t GetWorkers() { return num_workers_; }

    //
    // Callbacks called asynchronously by IO Queue library.
    // See queue/queue.h for description.
    //
    static zx_status_t cb_acquire(void* context, IoOp** op_list, size_t* op_count, bool wait) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        return test->AcquireOps(op_list, op_count, wait);
    }

    static zx_status_t cb_issue(void* context, IoOp* op) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        return test->IssueOp(op);
    }

    static void cb_release(void* context, IoOp* op) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        test->ReleaseOp(op);
    }

    static void cb_cancel_acquire(void* context) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        test->CancelAcquire();
    }

    static void cb_fatal(void* context) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        test->Fatal();
    }

private:
    zx_status_t AcquireOps(IoOp** op_list, size_t* op_count, bool wait);
    void CancelAcquire();
    zx_status_t IssueOp(IoOp* op);
    void ReleaseOp(IoOp* op);
    void Fatal();

    uint32_t num_workers_ = 1;
    IoQueue* q_ = nullptr;

    fbl::Mutex lock_;
    bool closed_ = false;
    uint32_t enqueued_count_ = 0;
    uint32_t issued_count_ = 0;
    uint32_t released_count_ = 0;
    list_node_t in_list_;
    fbl::ConditionVariable in_avail_;
    fbl::ConditionVariable acquired_all_;
    fbl::ConditionVariable released_all_;
};
