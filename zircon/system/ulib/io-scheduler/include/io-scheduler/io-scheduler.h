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

constexpr size_t kOpReservedQuads = 12;

struct IoSchedOp {
    uint32_t op_class;
    uint32_t flags;
    zx_status_t cookie;
    uint64_t _reserved[kOpReservedQuads];
};

struct IoSchedulerCallbacks {
    void* context;
    fbl::Function<bool(void* context, IoSchedOp* first, IoSchedOp* second)> CanReorder;
    fbl::Function<zx_status_t(void* context, IoSchedOp** sop_list, size_t list_count,
                              size_t* actual_count, bool wait)> Acquire;
    fbl::Function<zx_status_t(void* context, IoSchedOp* sop)> Issue;
    fbl::Function<void(void* context, IoSchedOp* sop)> Release;
    fbl::Function<void(void* context)> CancelAcquire;
    fbl::Function<void(void* context)> Fatal;
};

class IoSchedulerUniquePtr;

class IoScheduler {
public:
    IoScheduler() {};
    ~IoScheduler() {};

    static zx_status_t Create(IoSchedulerCallbacks* cb, IoSchedulerUniquePtr* out);
    static void Destroy(IoScheduler* sched);

    virtual zx_status_t Init();
    virtual void Shutdown();

    virtual zx_status_t StreamOpen(uint32_t id, uint32_t priority);
    virtual zx_status_t StreamClose(uint32_t id);
    virtual zx_status_t Serve();

    virtual void AsyncComplete();

private:
};

// A unique_ptr wrapper for IoScheduler
class IoSchedulerUniquePtr : public std::unique_ptr<IoScheduler, decltype(&IoScheduler::Destroy)> {
public:
    IoSchedulerUniquePtr(IoScheduler* sched = nullptr) :
        std::unique_ptr<IoScheduler, decltype(&IoScheduler::Destroy)>(sched, IoScheduler::Destroy) {
    }
};

} // namespace
