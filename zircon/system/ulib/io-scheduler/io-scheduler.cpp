// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <io-scheduler/io-scheduler.h>

namespace ioscheduler {

class Scheduler final : public IoScheduler {
public:
    Scheduler(IoSchedulerCallbacks* cb) : callbacks_(cb) {}
    ~Scheduler() {}

    virtual zx_status_t Init();
    virtual void Shutdown();

    virtual zx_status_t StreamOpen(uint32_t id, uint32_t priority);
    virtual zx_status_t StreamClose(uint32_t id);
    virtual zx_status_t Serve();

    virtual void AsyncComplete();

    void zzz() { callbacks_ = nullptr; }

private:
    IoSchedulerCallbacks* callbacks_;
};

zx_status_t Scheduler::Init() {
    return ZX_OK;
}

void Scheduler::Shutdown() { }

zx_status_t Scheduler::StreamOpen(uint32_t id, uint32_t priority) {
    return ZX_OK;
}

zx_status_t Scheduler::StreamClose(uint32_t id) {
    return ZX_OK;
}

zx_status_t Scheduler::Serve() {
    return ZX_OK;
}

void Scheduler::AsyncComplete() {

}

// IoScheduler implementation

zx_status_t IoScheduler::Create(IoSchedulerCallbacks* cb, IoSchedulerUniquePtr* out) {
    IoSchedulerUniquePtr ios(new Scheduler(cb));
    *out = std::move(ios);
    return ZX_OK;
}

void IoScheduler::Destroy(IoScheduler* sched) {
    Scheduler* s = static_cast<Scheduler*>(sched);
    delete s;
}

} // namespace
