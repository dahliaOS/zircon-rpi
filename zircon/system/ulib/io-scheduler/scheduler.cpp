// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <io-scheduler/io-scheduler.h>

#include "stream.h"

namespace ioscheduler {

class Scheduler {
public:
    Scheduler() {}
    ~Scheduler();

    zx_status_t Init(SchedulerCallbacks* cb, uint32_t options);
    void Shutdown();

    zx_status_t StreamOpen(uint32_t id, uint32_t priority);
    zx_status_t StreamClose(uint32_t id);
    zx_status_t Serve();

    void AsyncComplete(SchedOp* sop);

private:
    using StreamList = Stream::ListUnsorted;

    zx_status_t GetStreamForId(uint32_t id, StreamRef* out, bool remove);
    zx_status_t FindStreamForId(uint32_t id, StreamRef* out) {
        return GetStreamForId(id, out, false);
    }
    zx_status_t RemoveStreamForId(uint32_t id, StreamRef* out = nullptr) {
        return GetStreamForId(id, out, true);
    }

    SchedulerCallbacks* callbacks_ = nullptr;
    uint32_t options_ = 0;
    StreamList streams_;
};

zx_status_t Scheduler::Init(SchedulerCallbacks* cb, uint32_t options) {
    callbacks_ = cb;
    options_ = options;
    return ZX_OK;
}

void Scheduler::Shutdown() { }

zx_status_t Scheduler::StreamOpen(uint32_t id, uint32_t priority) {
    if (priority > kMaxPri) {
        return ZX_ERR_INVALID_ARGS;
    }

    StreamRef stream;
    zx_status_t status = FindStreamForId(id, &stream);
    if (status != ZX_ERR_NOT_FOUND) {
        return ZX_ERR_ALREADY_EXISTS;
    }

    fbl::AllocChecker ac;
    stream = fbl::AdoptRef(new (&ac) Stream(id, priority));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    streams_.push_back(std::move(stream));
    return ZX_OK;
}

zx_status_t Scheduler::StreamClose(uint32_t id) {
    return RemoveStreamForId(id);
}

zx_status_t Scheduler::Serve() {
    return ZX_OK;
}

void Scheduler::AsyncComplete(SchedOp* sop) {

}

Scheduler::~Scheduler() {
    Shutdown();
}

zx_status_t Scheduler::GetStreamForId(uint32_t id, StreamRef* out, bool remove) {
    for (auto iter = streams_.begin(); iter.IsValid(); ++iter) {
        if (iter->Id() == id) {
            if (out) {
                *out = iter.CopyPointer();
            }
            if (remove) {
                streams_.erase(iter);
            }
            return ZX_OK;
        }
    }
    return ZX_ERR_NOT_FOUND;
}

// Scheduler API

zx_status_t SchedulerCreate(Scheduler** out) {
    fbl::AllocChecker ac;
    Scheduler* sched = new (&ac) Scheduler();
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    *out = sched;
    return ZX_OK;
}

void SchedulerDestroy(Scheduler* scheduler) {
    delete scheduler;
}

zx_status_t SchedulerInit(Scheduler* scheduler, SchedulerCallbacks* cb, uint32_t options) {
    return scheduler->Init(cb, options);
}

zx_status_t SchedulerStreamOpen(Scheduler* scheduler, uint32_t id, uint32_t priority) {
    return scheduler->StreamOpen(id, priority);
}

zx_status_t SchedulerStreamClose(Scheduler* scheduler, uint32_t id) {
    return scheduler->StreamClose(id);
}

zx_status_t SchedulerServe(Scheduler* scheduler) {
   return scheduler->Serve();
}

void SchedulerShutdown(Scheduler* scheduler) {
   scheduler->Shutdown();
}

void AsyncComplete(Scheduler* scheduler, SchedOp* sop) {
   scheduler->AsyncComplete(sop);
}

} // namespace
