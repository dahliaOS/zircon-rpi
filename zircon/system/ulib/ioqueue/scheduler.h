// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <fbl/condition_variable.h>
#include <semaphore.h>
#include <ioqueue/queue.h>
#include <zircon/types.h>

#include "op.h"
#include "stream.h"

namespace ioqueue {

constexpr uint32_t kIoQueueNumPri = kIoQueueMaxPri + 1;
constexpr uint32_t kIoQueueMaxIssues = 1;   // TODO, make dynamic

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    zx_status_t AddStream(StreamRef stream) __TA_EXCLUDES(lock_);
    StreamRef FindStream(uint32_t id) __TA_EXCLUDES(lock_);
    void RemoveStream(StreamRef stream) __TA_EXCLUDES(lock_);
    void RemoveStreamLocked(StreamRef stream) __TA_REQUIRES(lock_);

    zx_status_t InsertOps(Op** op_list, size_t op_count, size_t* out_num_ready) __TA_EXCLUDES(lock_);
    zx_status_t GetNextOp(bool wait, Op** op_out) __TA_EXCLUDES(lock_);
    zx_status_t GetCompletedOps(Op** op_list, size_t op_count, size_t* out_count) __TA_EXCLUDES(lock_);
    void CompleteOp(Op* op, bool async) __TA_EXCLUDES(lock_);

    void CloseAll() __TA_EXCLUDES(lock_);
    void WaitUntilDrained() __TA_EXCLUDES(lock_);

    uint32_t NumReadyOps() __TA_EXCLUDES(lock_);

private:
    using StreamIdMap = Stream::WAVLTreeSortById;
    using StreamList = Stream::ListUnsorted;

    StreamRef FindStreamLocked(uint32_t id) __TA_REQUIRES(lock_);

    fbl::Mutex lock_;
    fbl::ConditionVariable event_drained_ __TA_GUARDED(lock_);   // All ops have been consumed.
    sem_t issue_sem_ __TA_GUARDED(lock_);                // Number of concurrent issues.
    uint32_t num_streams_ __TA_GUARDED(lock_) = 0;       // Number of streams in the priority list.
    uint32_t num_ready_ops_ __TA_GUARDED(lock_) = 0;     // Number of ops waiting to be issued.
    uint32_t num_issued_ops_ __TA_GUARDED(lock_) = 0;    // Number of issued ops.
    uint32_t max_issues_ __TA_GUARDED(lock_);            // Maximum number of concurrent issues.
    list_node_t completed_op_list_ __TA_GUARDED(lock_);
    StreamIdMap stream_map_ __TA_GUARDED(lock_);         // Map of id to stream.
    StreamList pri_list_[kIoQueueNumPri] __TA_GUARDED(lock_);
};

} // namespace
