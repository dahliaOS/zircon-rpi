// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <fbl/condition_variable.h>
#include <semaphore.h>
#include <zircon/types.h>

#include "io-op.h"
#include "io-stream.h"

#define IO_SCHED_DEFAULT_PRI       16
#define IO_SCHED_NUM_PRI           32
#define IO_SCHED_MAX_PRI           (IO_SCHED_NUM_PRI - 1)

#define SCHED_OPS_HIWAT 20
#define SCHED_OPS_LOWAT 5
#define SCHED_MAX_ISSUES 1 // TODO, make dynamic

namespace ioqueue {

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    zx_status_t AddStream(StreamRef stream);
    StreamRef FindStream(uint32_t id);
    void RemoveStream(StreamRef stream);
    void RemoveStreamLocked(StreamRef stream);

    zx_status_t InsertOps(io_op_t** op_list, size_t op_count, size_t* out_num_ready);
    zx_status_t GetNextOp(bool wait, io_op_t** op_out);
    zx_status_t GetCompletedOps(io_op_t** op_list, size_t op_count, size_t* out_count);
    void CompleteOp(io_op_t* op, zx_status_t result);


    void CloseAll();
    void WaitUntilDrained();

    uint32_t NumReadyOps();

private:
    using StreamIdMap = Stream::WAVLTreeSortById;
    using StreamList = Stream::ListUnsorted;

    StreamRef FindStreamLocked(uint32_t id);

    fbl::Mutex lock_;
//    cnd_t event_available_;          // New ops are available.
    fbl::ConditionVariable event_drained_;   // All ops have been consumed.
    sem_t issue_sem_;                // Number of concurrent issues.
    uint32_t num_streams_ = 0;       // Number of streams in the priority list.
    uint32_t num_ready_ops_ = 0;     // Number of ops waiting to be issued.
    uint32_t num_issued_ops_ = 0;    // Number of issued ops.
    uint32_t max_issues_;            // Maximum number of concurrent issues.
    list_node_t completed_op_list_;

    StreamIdMap stream_map_;         // Map of id to stream.
    StreamList pri_list_[IO_SCHED_NUM_PRI];
};

} // namespace
