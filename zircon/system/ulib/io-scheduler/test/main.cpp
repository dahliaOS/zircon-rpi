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
#include <unittest/unittest.h>

namespace tests {

using IoScheduler = ioscheduler::Scheduler;
using IoSchedulerUniquePtr = ioscheduler::SchedulerUniquePtr;
using SchedOp = ioscheduler::SchedOp;

constexpr uint32_t kMaxFifoDepth = (4096 / sizeof(void*));

struct TestOp {

};

class TestContext {
public:
    TestContext() {}
    ~TestContext() {}

    IoScheduler* Scheduler() { return sched_; }
    void SetScheduler(IoScheduler* sched) { sched_ = sched; }
    zx_status_t CreateFifo();

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

    static bool CanReorder(void* context, SchedOp* first, SchedOp* second) {
        return false;
    }

    static zx_status_t AcquireCallback(void* context, SchedOp** sop_list, size_t list_count,
                                       size_t* actual_count, bool wait) {
        TestContext* test = static_cast<TestContext*>(context);
        return test->AcquireOps(sop_list, list_count, actual_count, wait);
    }

    static zx_status_t IssueCallback(void* context, SchedOp* sop) {
        TestContext* test = static_cast<TestContext*>(context);
        return test->IssueOp(sop);
    }

    static void ReleaseCallback(void* context, SchedOp* sop) {
        TestContext* test = static_cast<TestContext*>(context);
        test->ReleaseOp(sop);
    }

    static void CancelCallback(void* context) {
        TestContext* test = static_cast<TestContext*>(context);
        test->CancelAcquire();
    }

    static void FatalCallback(void* context) {
        TestContext* test = static_cast<TestContext*>(context);
        test->Fatal();
    }

private:
    IoScheduler* sched_;
    fzl::fifo<void*, void*> fifo_to_server_;
    fzl::fifo<void*, void*> fifo_from_server_;
};

zx_status_t TestContext::CreateFifo() {
    return fzl::create_fifo(kMaxFifoDepth, 0, &fifo_to_server_, &fifo_from_server_);
}

ioscheduler::SchedulerCallbacks callbacks = {
    .context = nullptr,
    .CanReorder = TestContext::CanReorder,
    .Acquire = TestContext::AcquireCallback,
    .Issue = TestContext::IssueCallback,
    .Release = TestContext::ReleaseCallback,
    .CancelAcquire = TestContext::CancelCallback,
    .Fatal = TestContext::FatalCallback,
};

enum TestLevel {
    kTestLevelCreate,
    kTestLevelInit,
    kTestLevelOpen,
    kTestLevelServe,
};

bool iosched_up(TestLevel test_level, TestContext* test) {
    zx_status_t status;
    IoScheduler* sched = test->Scheduler();
    // Create test.
    // --------------------------------
    if (test_level == kTestLevelCreate) return true;

    // Init test.
    // --------------------------------
    status = ioscheduler::SchedulerInit(sched, &callbacks, ioscheduler::kSchedOptStrictlyOrdered);
    ASSERT_EQ(status, ZX_OK, "Failed to init scheduler");

    if (test_level == kTestLevelInit) return true;

    // Stream open test.
    // --------------------------------
    status = ioscheduler::SchedulerStreamOpen(sched, 5, ioscheduler::kDefaultPri);
    ASSERT_EQ(status, ZX_OK, "Failed to open stream");
    status = ioscheduler::SchedulerStreamOpen(sched, 0, ioscheduler::kDefaultPri);
    ASSERT_EQ(status, ZX_OK, "Failed to open stream");
    status = ioscheduler::SchedulerStreamOpen(sched, 5, ioscheduler::kDefaultPri);
    ASSERT_NE(status, ZX_OK, "Expected failure to open duplicate stream");
    status = ioscheduler::SchedulerStreamOpen(sched, 3, 100000);
    ASSERT_NE(status, ZX_OK, "Expected failure to open with invalid priority");
    status = ioscheduler::SchedulerStreamOpen(sched, 3, 1);
    ASSERT_EQ(status, ZX_OK, "Failed to open stream");

    if (test_level == kTestLevelOpen) return true;

    // Serve test.
    // --------------------------------
    status = test->CreateFifo();
    ASSERT_EQ(status, ZX_OK, "Internal test failure");
    status = ioscheduler::SchedulerServe(sched);
    ASSERT_EQ(status, ZX_OK, "Failed to begin service");
    if (test_level == kTestLevelServe) return true;

    ASSERT_TRUE(false, "Unexpected test level");
    return false;
}

bool iosched_down(TestLevel test_level, TestContext* test) {
    zx_status_t status;
    IoScheduler* sched = test->Scheduler();
    switch (test_level) {
    case kTestLevelServe:
        // Serve test.
        // --------------------------------
    case kTestLevelOpen:
        // Stream open test.
        // --------------------------------
        status = ioscheduler::SchedulerStreamClose(sched, 5);
        ASSERT_EQ(status, ZX_OK, "Failed to close stream");
        status = ioscheduler::SchedulerStreamClose(sched, 3);
        ASSERT_EQ(status, ZX_OK, "Failed to close stream");
        // Stream 0 intentionally left open here.
        __FALLTHROUGH;
    case kTestLevelInit:
        // Init test.
        // --------------------------------
        ioscheduler::SchedulerShutdown(sched);
        __FALLTHROUGH;
    case kTestLevelCreate:
        // Create test.
        // --------------------------------
        break;
    default:
        ASSERT_TRUE(false, "Unexpected test level");
        return false;
    }
    return true;
}

bool iosched_run(TestLevel test_level) {
    BEGIN_TEST;

    TestContext test;
    callbacks.context = &test;
    IoScheduler* scheduler;
    zx_status_t status = ioscheduler::SchedulerCreate(&scheduler);
    ASSERT_EQ(status, ZX_OK, "Failed to create scheduler");
    test.SetScheduler(scheduler);
    IoSchedulerUniquePtr sched(scheduler);

    iosched_up(test_level, &test);

    iosched_down(test_level, &test);

    sched.release();
    END_TEST;
}

static bool iosched_test_create() {
    return iosched_run(kTestLevelCreate);
}

static bool iosched_test_open() {
    return iosched_run(kTestLevelOpen);
}

static bool iosched_test_serve() {
    return iosched_run(kTestLevelServe);
}

BEGIN_TEST_CASE(test_case_iosched)
RUN_TEST(iosched_test_create)
RUN_TEST(iosched_test_open)
RUN_TEST(iosched_test_serve)
END_TEST_CASE(test_case_iosched)

} // namespace tests

int main(int argc, char** argv) {
    bool success = unittest_run_all_tests(argc, argv);
    return success ? 0 : -1;
}
