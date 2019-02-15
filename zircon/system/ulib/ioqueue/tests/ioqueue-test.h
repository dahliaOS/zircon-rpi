// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fbl/condition_variable.h>
#include <ioqueue/queue.h>
#include <zircon/listnode.h>

struct TestOp {
    IoOp op;
    list_node_t node;
    uint32_t id;
    bool issued;
    bool released;
};

class IoQueueTest {
public:
    IoQueueTest();

    void Enqueue(TestOp* top);
    void SetQueue(IoQueue* q) { q_ = q; }
    IoQueue* GetQueue() { return q_; }
    void CloseInput(bool wait);
    void GetCounts(uint32_t counts[3]);

    // Callbacks
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

    IoQueue* q_ = nullptr;

    fbl::Mutex lock_;
    bool closed_ = false;
    uint32_t enqueued_count_ = 0;
    uint32_t issued_count_ = 0;
    uint32_t released_count_ = 0;
    list_node_t in_list_;
    fbl::ConditionVariable in_avail_;
    fbl::ConditionVariable released_all_;
};
