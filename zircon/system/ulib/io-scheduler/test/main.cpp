// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fbl/auto_lock.h>
#include <io-scheduler/io-scheduler.h>
#include <unittest/unittest.h>

namespace tests {

using IoScheduler = ioscheduler::IoScheduler;
using IoSchedulerUniquePtr = ioscheduler::IoSchedulerUniquePtr;
using IoSchedOp = ioscheduler::IoSchedOp;

static bool can_reorder(void* context, IoSchedOp* first, IoSchedOp* second) {
    return false;
}

static zx_status_t acquire(void* context, IoSchedOp** sop_list, size_t list_count,
                           size_t* actual_count, bool wait) {
    return ZX_OK;
}

static zx_status_t issue(void* context, IoSchedOp* sop) {
    return ZX_OK;
}

static void release(void* context, IoSchedOp* sop) { }
static void cancel(void* context) { }
static void fatal(void* context) { }

ioscheduler::IoSchedulerCallbacks callbacks = {
    .context = nullptr,
    .CanReorder = can_reorder,
    .Acquire = acquire,
    .Issue = issue,
    .Release = release,
    .CancelAcquire = cancel,
    .Fatal = fatal,
};

enum TestLevel {
    kTestLevelCreate,
    kTestLevelInit,
    kTestLevelOpen,
    kTestLevelServe,
};

static bool iosched_run(TestLevel test_level) {
    BEGIN_TEST;

    IoSchedulerUniquePtr sched;
    zx_status_t status = IoScheduler::Create(&callbacks, &sched);
    ASSERT_EQ(status, ZX_OK, "Failed to create scheduler");

    do {
        if (test_level == kTestLevelCreate) break;

        // Init test.
        status = sched->Init();
        ASSERT_EQ(status, ZX_OK, "Failed to init scheduler");
        if (test_level == kTestLevelInit) break;

        // Stream open test.
        status = sched->StreamOpen(5, ioscheduler::kDefaultPri);
        ASSERT_EQ(status, ZX_OK, "Failed to open stream");
        if (test_level == kTestLevelOpen) break;

        // Serve test.
        status = sched->Serve();
        ASSERT_EQ(status, ZX_OK, "Failed to being service");
        if (test_level == kTestLevelServe) break;

        ASSERT_TRUE(false, "Unexpected test level");
    } while (false);

    switch (test_level) {
    case kTestLevelServe:
    case kTestLevelOpen:
        status = sched->StreamClose(5);
        ASSERT_EQ(status, ZX_OK, "Failed to close stream");
        __FALLTHROUGH;
    case kTestLevelInit:
        sched->Shutdown();
        __FALLTHROUGH;
    case kTestLevelCreate:
        break;
    default:
        ASSERT_TRUE(false, "Unexpected test level");
    }

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
