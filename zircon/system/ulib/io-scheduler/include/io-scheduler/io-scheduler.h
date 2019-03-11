// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <stdint.h>

#include <fbl/function.h>
#include <zircon/types.h>

namespace ioscheduler {

constexpr uint32_t kMaxPri = 31;
constexpr uint32_t kDefaultPri = 8;

constexpr uint32_t kOpClassUnknown = 0;
constexpr uint32_t kOpClassRead = 1;
constexpr uint32_t kOpClassWrite = 2;
constexpr uint32_t kOpClassDiscard = 3;
constexpr uint32_t kOpClassRename = 4;
constexpr uint32_t kOpClassSync = 5;
constexpr uint32_t kOpClassReadBarrier = 64;
constexpr uint32_t kOpClassWriteBarrier = 65;
constexpr uint32_t kOpClassFullBarrier = 66;

constexpr size_t kOpReservedQuads = 13;

struct SchedOp {
    uint32_t op_class;
    uint32_t flags;
    uint32_t _unused;
    zx_status_t result;
    void* cookie;
    uint64_t _reserved[kOpReservedQuads];
};

struct SchedulerCallbacks {
    void* context;
    fbl::Function<bool(void* context, SchedOp* first, SchedOp* second)> CanReorder;
    fbl::Function<zx_status_t(void* context, SchedOp** sop_list, size_t list_count,
                              size_t* actual_count, bool wait)> Acquire;
    fbl::Function<zx_status_t(void* context, SchedOp* sop)> Issue;
    fbl::Function<void(void* context, SchedOp* sop)> Release;
    fbl::Function<void(void* context)> CancelAcquire;
    fbl::Function<void(void* context)> Fatal;
};

class Scheduler;

zx_status_t SchedulerCreate(SchedulerCallbacks* cb, Scheduler** out);
zx_status_t SchedulerInit(Scheduler* scheduler);
zx_status_t SchedulerStreamOpen(Scheduler* scheduler, uint32_t id, uint32_t priority);
zx_status_t SchedulerStreamClose(Scheduler* scheduler, uint32_t id);
zx_status_t SchedulerServe(Scheduler* scheduler);
void SchedulerShutdown(Scheduler* scheduler);
void SchedulerDestroy(Scheduler* scheduler);

void AsyncComplete(Scheduler* scheduler, SchedOp* sop);

class SchedulerUniquePtr : public std::unique_ptr<Scheduler, decltype(&SchedulerDestroy)> {
public:
    SchedulerUniquePtr(Scheduler* sched = nullptr) :
        std::unique_ptr<Scheduler, decltype(&SchedulerDestroy)>(sched, SchedulerDestroy) {
    }
};

} // namespace
