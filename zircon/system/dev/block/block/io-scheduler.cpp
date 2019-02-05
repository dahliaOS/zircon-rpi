// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fbl/auto_lock.h>

#include "io-scheduler.h"

namespace ioqueue {

static inline io_op_t* node_to_op(list_node_t* node) {
    return containerof(node, io_op_t, node);
}

Scheduler::Scheduler() {
    sem_init(&issue_sem_, 0, SCHED_MAX_ISSUES);
    max_issues_ = SCHED_MAX_ISSUES;     // Todo: make dynamic.
    list_initialize(&completed_op_list_);
}

Scheduler::~Scheduler() {
    fbl::AutoLock lock(&lock_);
    assert(num_ready_ops_ == 0);
    assert(num_issued_ops_ == 0);
    assert(list_is_empty(&completed_op_list_));

    // Delete remaining streams.
    for ( ; ; ) {
        StreamRef stream = stream_map_.pop_front();
        if (stream == nullptr) break;
        fbl::AutoLock stream_lock(&stream->lock_);
        assert(stream->flags_ & kIoStreamFlagClosed);
        assert((stream->flags_ & kIoStreamFlagScheduled) == 0);
    }
    sem_destroy(&issue_sem_);
}

zx_status_t Scheduler::AddStream(StreamRef stream) {
    fbl::AutoLock lock(&lock_);
    ZX_DEBUG_ASSERT(stream_map_.find(stream->id_).IsValid() == false);
    stream_map_.insert(std::move(stream));
    return ZX_OK;
}

StreamRef Scheduler::FindStream(uint32_t id) {
    fbl::AutoLock lock(&lock_);
    return FindStreamLocked(id);
}

StreamRef Scheduler::FindStreamLocked(uint32_t id) {
    auto iter = stream_map_.find(id);
    if (!iter.IsValid()) {
        return nullptr;
    }
    return iter.CopyPointer();
}

void Scheduler::RemoveStream(StreamRef stream) {
    fbl::AutoLock lock(&lock_);
    RemoveStreamLocked(std::move(stream));
}

void Scheduler::RemoveStreamLocked(StreamRef stream) {
    stream_map_.erase(stream->id_);
}

zx_status_t Scheduler::InsertOps(io_op_t** op_list, size_t op_count, size_t* out_num_ready) {
    bool was_empty = true;
    bool stream_added = false;
    fbl::AutoLock lock(&lock_);
    if (num_streams_ > 0) {
        was_empty = false;
    }
    zx_status_t status = ZX_OK;
    for (size_t i = 0; i < op_count; i++) {
        io_op_t* op = op_list[i];
        // TODO(sron): avoid redundant lookups of same sid.
        StreamRef stream = FindStreamLocked(op->sid);
        if (stream == nullptr) {
            fprintf(stderr, "Error: Attempted to enqueue op for non-existent stream\n");
            op->result = ZX_ERR_INVALID_ARGS;
            status = ZX_ERR_INVALID_ARGS;
            continue;
        }
        fbl::AutoLock stream_lock(&stream->lock_);
        if (stream->flags_ & kIoStreamFlagClosed) {
            stream_lock.release();
            fprintf(stderr, "Error: attempted to enqueue op for closed stream\n");
            op->result = ZX_ERR_INVALID_ARGS;
            status = ZX_ERR_INVALID_ARGS;
            continue;
        }
        op_list[i] = nullptr; // Clear out inserted ops.
        list_clear_node(&op->node);
        list_add_tail(&stream->ready_op_list_, &op->node);
        if ((stream->flags_ & kIoStreamFlagScheduled) == 0) {
            stream->flags_ |= kIoStreamFlagScheduled;
            pri_list_[stream->priority_].push_back(std::move(stream));
            num_streams_++;
            stream_added = true;
        }
        num_ready_ops_++;
    }
    // if (was_empty && stream_added) {
    //     cnd_broadcast(&event_issue_available_);
    // }
    if (out_num_ready) {
        *out_num_ready = num_ready_ops_;
    }
    return status;
}

zx_status_t Scheduler::GetNextOp(bool wait, io_op_t** op_out) {
    for ( ; ; ) {
        int err;
        if (wait) {
            err = sem_wait(&issue_sem_);
        } else {
            err = sem_trywait(&issue_sem_);
        }
        if (err == 0) {
            break;
        }
        int eno = errno;
        if ((!wait) && (eno == EAGAIN)) {
            return ZX_ERR_SHOULD_WAIT;
        }
        ZX_DEBUG_ASSERT(eno == EINTR);
    }
    // Holding an issue slot.
    fbl::AutoLock lock(&lock_);
    if (num_ready_ops_ == 0) {
        sem_post(&issue_sem_);
        return ZX_ERR_UNAVAILABLE;
    }
    // Locate the first op in priority list
    StreamRef stream;
    for (uint32_t i = 0; i < IO_SCHED_NUM_PRI; i++) {
        uint32_t pri = IO_SCHED_MAX_PRI - i;
        stream = pri_list_[pri].pop_front();
        if (stream != nullptr) {
            break;
        }
    }
    assert(stream != nullptr);
    fbl::AutoLock stream_lock(&stream->lock_);
    list_node_t* op_node = list_remove_head(&stream->ready_op_list_);
    assert(op_node != nullptr);
    // Move to issued list
    list_add_tail(&stream->issued_op_list_, op_node);
    num_ready_ops_--;
    num_issued_ops_++;
    io_op_t* op = node_to_op(op_node);
    if (list_is_empty(&stream->ready_op_list_)) {
        // Do not reinsert into queue.
        stream->flags_ &= ~kIoStreamFlagScheduled;
        num_streams_--;
        stream->event_unscheduled_.Broadcast();
    } else {
        // Insert to back of list of streams at this priority.
        pri_list_[stream->priority_].push_back(std::move(stream));
    }
    *op_out = op;
    return ZX_OK;
}

zx_status_t Scheduler::GetCompletedOps(io_op_t** op_list, size_t op_count, size_t* out_count) {
    fbl::AutoLock lock(&lock_);
    size_t i;
    for (i = 0; i < op_count; i++) {
        list_node_t* op_node = list_remove_head(&completed_op_list_);
        if (op_node == nullptr) {
            break;
        }
        op_list[i] = node_to_op(op_node);
    }
    *out_count = i;
    return ZX_OK;
}

void Scheduler::CompleteOp(io_op_t* op, zx_status_t result) {
    fbl::AutoLock lock(&lock_);
    num_issued_ops_--;
    sem_post(&issue_sem_);
    StreamRef stream = FindStreamLocked(op->sid);
    if (stream == NULL) {
        fprintf(stderr, "Error: completed op for non-existent stream %u\n", op->sid);
        op->result = ZX_ERR_INVALID_ARGS;
        return;
    }
    fbl::AutoLock stream_lock(&stream->lock_);
    op->result = result;
    list_delete(&op->node);  // Remove from issued list.
    list_add_tail(&completed_op_list_, &op->node); // Add to list of ops pending completion.
}

// Close all streams.
void Scheduler::CloseAll() {
    fbl::AutoLock lock(&lock_);
    for (auto& stream : stream_map_) {
        fbl::AutoLock stream_lock(&stream.lock_);
        stream.flags_ |= kIoStreamFlagClosed;
    }
}

void Scheduler::WaitUntilDrained() {
    fbl::AutoLock lock(&lock_);
    if (num_streams_ > 0) {
        event_drained_.Wait(&lock_);
        assert(num_streams_ == 0);
    }
}

uint32_t Scheduler::NumReadyOps() {
    fbl::AutoLock lock(&lock_);
    return num_ready_ops_;
}

} // namespace
