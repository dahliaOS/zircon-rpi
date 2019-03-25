// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fbl/auto_lock.h>
#include <io-scheduler/io-scheduler.h>
#include <lib/fzl/fifo.h>
#include <zxtest/zxtest.h>

namespace {

using IoScheduler = ioscheduler::Scheduler;
using IoSchedulerUniquePtr = ioscheduler::SchedulerUniquePtr;
using SchedOp = ioscheduler::SchedOp;

constexpr uint32_t kMaxFifoDepth = (4096 / sizeof(void*));

class IOSchedTestFixture : public zxtest::Test {
public:

protected:
    // Called before every test of this test case.
    void SetUp() override {
        ASSERT_EQ(sched_, nullptr);
        ASSERT_OK(ioscheduler::SchedulerCreate(&sched_), "Failed to create scheduler");
        callbacks_.context = this;
    }

    // Called after every test of this test case.
    void TearDown() override {
        if (sched_ != nullptr) {
            SchedulerDestroy(sched_);
            sched_ = nullptr;
        }
    }

    IoScheduler* Scheduler() { return sched_; }

protected:
    void CreateFifo() {
        ASSERT_OK(fzl::create_fifo(kMaxFifoDepth, 0, &fifo_to_server_, &fifo_from_server_),
                  "Failed to create FIFOs");
    }


    // Callback methods.
    zx_status_t AcquireOps(SchedOp** sop_list, size_t list_count,
                           size_t* actual_count, bool wait) {
        return ZX_OK;
    }

    zx_status_t IssueOp(SchedOp* sop) {
        return ZX_OK;
    }

    zx_status_t ReleaseOp(SchedOp* sop) {
        return ZX_OK;
    }

    void CancelAcquire() {}
    void Fatal() {}

    // Callback interface functions.
    static bool CanReorderCallback(void* context, SchedOp* first, SchedOp* second) {
        return false;
    }

    static zx_status_t AcquireCallback(void* context, SchedOp** sop_list, size_t list_count,
                                       size_t* actual_count, bool wait) {
        IOSchedTestFixture* fix = static_cast<IOSchedTestFixture*>(context);
        return fix->AcquireOps(sop_list, list_count, actual_count, wait);
    }

    static zx_status_t IssueCallback(void* context, SchedOp* sop) {
        IOSchedTestFixture* fix = static_cast<IOSchedTestFixture*>(context);
        return fix->IssueOp(sop);
    }

    static void ReleaseCallback(void* context, SchedOp* sop) {
        IOSchedTestFixture* fix = static_cast<IOSchedTestFixture*>(context);
        fix->ReleaseOp(sop);
    }

    static void CancelCallback(void* context) {
        IOSchedTestFixture* fix = static_cast<IOSchedTestFixture*>(context);
        fix->CancelAcquire();
    }

    static void FatalCallback(void* context) {
        IOSchedTestFixture* fix = static_cast<IOSchedTestFixture*>(context);
        fix->Fatal();
    }

    IoScheduler* sched_ = nullptr;
    fzl::fifo<void*, void*> fifo_to_server_;
    fzl::fifo<void*, void*> fifo_from_server_;

    ioscheduler::SchedulerCallbacks callbacks_ = {
        .context = nullptr,
        .CanReorder = CanReorderCallback,
        .Acquire = AcquireCallback,
        .Issue = IssueCallback,
        .Release = ReleaseCallback,
        .CancelAcquire = CancelCallback,
        .Fatal = FatalCallback,
    };

private:
};

// Create and destroy scheduler.
TEST_F(IOSchedTestFixture, CreateTest) {
    ASSERT_TRUE(true);
}

// Init scheduler.
TEST_F(IOSchedTestFixture, InitTest) {
    zx_status_t status = ioscheduler::SchedulerInit(sched_, &callbacks_,
                                                    ioscheduler::kSchedOptStrictlyOrdered);
    ASSERT_OK(status, "Failed to init scheduler");
    ioscheduler::SchedulerShutdown(sched_);
}

// Open streams.
TEST_F(IOSchedTestFixture, OpenTest) {
    zx_status_t status = ioscheduler::SchedulerInit(sched_, &callbacks_,
                                                    ioscheduler::kSchedOptStrictlyOrdered);
    ASSERT_OK(status, "Failed to init scheduler");

    // Open streams.
    status = ioscheduler::SchedulerStreamOpen(sched_, 5, ioscheduler::kDefaultPri);
    ASSERT_OK(status, "Failed to open stream");
    status = ioscheduler::SchedulerStreamOpen(sched_, 0, ioscheduler::kDefaultPri);
    ASSERT_OK(status, "Failed to open stream");
    status = ioscheduler::SchedulerStreamOpen(sched_, 5, ioscheduler::kDefaultPri);
    ASSERT_NOT_OK(status, "Expected failure to open duplicate stream");
    status = ioscheduler::SchedulerStreamOpen(sched_, 3, 100000);
    ASSERT_NOT_OK(status, "Expected failure to open with invalid priority");
    status = ioscheduler::SchedulerStreamOpen(sched_, 3, 1);
    ASSERT_OK(status, "Failed to open stream");

    // Close streams.
    status = ioscheduler::SchedulerStreamClose(sched_, 5);
    ASSERT_OK(status, "Failed to close stream");
    status = ioscheduler::SchedulerStreamClose(sched_, 3);
    ASSERT_OK(status, "Failed to close stream");
    // Stream 0 intentionally left open here.

    ioscheduler::SchedulerShutdown(sched_);
}

// Serve.
TEST_F(IOSchedTestFixture, ServeTest) {
    zx_status_t status = ioscheduler::SchedulerInit(sched_, &callbacks_,
                                                    ioscheduler::kSchedOptStrictlyOrdered);
    ASSERT_OK(status, "Failed to init scheduler");
    status = ioscheduler::SchedulerStreamOpen(sched_, 0, ioscheduler::kDefaultPri);
    ASSERT_OK(status, "Failed to open stream");

    ASSERT_NO_FATAL_FAILURES(CreateFifo(), "Internal test failure");
    ASSERT_OK(ioscheduler::SchedulerServe(sched_), "Failed to begin service");

    // TODO: insert ops into FIFO.

    status = ioscheduler::SchedulerStreamClose(sched_, 0);
    ASSERT_OK(status, "Failed to close stream");
    ioscheduler::SchedulerShutdown(sched_);
}

} // namespace


int main(int argc, char* argv[]) {
    return RUN_ALL_TESTS(argc, argv);
    return 0;
}
