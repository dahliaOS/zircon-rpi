// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdio.h>

#include <fbl/auto_lock.h>

#include "ioqueue-test.h"

IoQueueTest::IoQueueTest(uint32_t num_workers) : num_workers_(num_workers) {
    list_initialize(&in_list_);
}

void IoQueueTest::Enqueue(TestOp* top) {
    fbl::AutoLock lock(&lock_);
    top->enqueued = true;
    list_add_tail(&in_list_, &top->node);
    in_avail_.Signal();
}

void IoQueueTest::GetCounts(uint32_t count[3]) {
    fbl::AutoLock lock(&lock_);
    count[0] = enqueued_count_;
    count[1] = issued_count_;
    count[2] = released_count_;
}

void IoQueueTest::WaitForAcquired() {
    fbl::AutoLock lock(&lock_);
    while (!list_is_empty(&in_list_)) {
        acquired_all_.Wait(&lock_);
    }
}

void IoQueueTest::WaitForReleased() {
    fbl::AutoLock lock(&lock_);
    while (!list_is_empty(&in_list_)) {
        acquired_all_.Wait(&lock_);
    }
    while (released_count_ != enqueued_count_) {
        released_all_.Wait(&lock_);
    }
}

void IoQueueTest::CloseInput() {
    fbl::AutoLock lock(&lock_);
    closed_ = true;
    in_avail_.Broadcast();
}

zx_status_t IoQueueTest::AcquireOps(IoOp** op_list, size_t* op_count, bool wait) {
    fbl::AutoLock lock(&lock_);
    // printf("cb: acquire\n");
    if (closed_) {
        // printf("cb:   closed\n");
        return ZX_ERR_CANCELED;   // Input source closed.
    }
    if (list_is_empty(&in_list_)) {
        if (!wait) {
            return ZX_ERR_SHOULD_WAIT;
        }
        in_avail_.Wait(&lock_);
        if (closed_) {
            return ZX_ERR_CANCELED;
        }
    }
    size_t i, max_ops = *op_count;
    for (i = 0; i < max_ops; i++) {
        list_node_t* node = list_remove_head(&in_list_);
        if (node == nullptr) {
            break;
        }
        TestOp* top = containerof(node, TestOp, node);
        op_list[i] = &top->op;
        // printf("cb: acquire %u:%u\n", top->op.stream_id, top->id);
        enqueued_count_++;
    }
    *op_count = i;
    if (list_is_empty(&in_list_)) {
        acquired_all_.Broadcast();
    }
    return ZX_OK;
}

zx_status_t IoQueueTest::IssueOp(IoOp* op) {
    // printf("cb: issue %p\n", op);
    TestOp* top = containerof(op, TestOp, op);
    // printf("cb: issue %u:%u\n", op->stream_id, top->id);
    fbl::AutoLock lock(&lock_);
    assert(top->issued == false);
    top->issued = true;
    op->result = ZX_OK;
    issued_count_++;
    return ZX_OK;
}

void IoQueueTest::ReleaseOp(IoOp* op) {
    // printf("cb: release %p\n", op);
    TestOp* top = containerof(op, TestOp, op);
    // printf("cb: release %u:%u\n", op->stream_id, top->id);
    fbl::AutoLock lock(&lock_);
    assert(top->released == false);
    top->released = true;
    released_count_++;
    if (released_count_ == enqueued_count_) {
        released_all_.Broadcast();
    }
}

void IoQueueTest::CancelAcquire() {
    // printf("cb: cancel_acquire\n");
    CloseInput();
}

void IoQueueTest::Fatal() {
    // printf("cb: FATAL\n");
    assert(false);
}
