// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <io-scheduler/io-scheduler.h>

#include "stream.h"

namespace ioscheduler {

class Scheduler final : public IoScheduler {
public:
    Scheduler(IoSchedulerCallbacks* cb) : callbacks_(cb) {}
    ~Scheduler();

    virtual zx_status_t Init();
    virtual void Shutdown();

    virtual zx_status_t StreamOpen(uint32_t id, uint32_t priority);
    virtual zx_status_t StreamClose(uint32_t id);
    virtual zx_status_t Serve();

    virtual void AsyncComplete();

    void zzz() { callbacks_ = nullptr; }

private:
    using StreamList = Stream::ListUnsorted;

    zx_status_t FindStreamForId(uint32_t id, StreamRef* out);
    zx_status_t RemoveStreamForId(uint32_t id, StreamRef* out = nullptr);

    IoSchedulerCallbacks* callbacks_;
    StreamList streams_;
};

zx_status_t Scheduler::Init() {
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

void Scheduler::AsyncComplete() {

}

Scheduler::~Scheduler() {
    Shutdown();
}

zx_status_t Scheduler::FindStreamForId(uint32_t id, StreamRef* out) {
    for (auto iter = streams_.begin(); iter.IsValid(); ++iter) {
        if (iter->Id() == id) {
            *out = iter.CopyPointer();
            return ZX_OK;
        }
    }
    return ZX_ERR_NOT_FOUND;
}

zx_status_t Scheduler::RemoveStreamForId(uint32_t id, StreamRef* out) {
    for (auto iter = streams_.begin(); iter.IsValid(); ++iter) {
        if (iter->Id() == id) {
            if (out) {
                *out = iter.CopyPointer();
            }
            streams_.erase(iter);
            return ZX_OK;
        }
    }
    return ZX_ERR_NOT_FOUND;
}


// IoScheduler implementation

zx_status_t IoScheduler::Create(IoSchedulerCallbacks* cb, IoSchedulerUniquePtr* out) {
    fbl::AllocChecker ac;
    IoSchedulerUniquePtr ios(new (&ac) Scheduler(cb));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    *out = std::move(ios);
    return ZX_OK;
}

void IoScheduler::Destroy(IoScheduler* sched) {
    Scheduler* s = static_cast<Scheduler*>(sched);
    delete s;
}

} // namespace
