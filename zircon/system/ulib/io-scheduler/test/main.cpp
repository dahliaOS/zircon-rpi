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
using IoSchedulerCallbacks = ioscheduler::IoSchedulerCallbacks;

IoSchedulerCallbacks callbacks;

static bool iosched_test_create() {
    BEGIN_TEST;

    IoSchedulerUniquePtr sched;
    zx_status_t status = IoScheduler::Create(&callbacks, &sched);
    ASSERT_EQ(status, ZX_OK, "Failed to create scheduler");

    sched.release();
    END_TEST;
}

static bool iosched_test_open() {
    BEGIN_TEST;

    IoSchedulerUniquePtr sched;
    zx_status_t status = IoScheduler::Create(&callbacks, &sched);
    ASSERT_EQ(status, ZX_OK, "Failed to create scheduler");

    status = sched->Init();
    ASSERT_EQ(status, ZX_OK, "Failed to init scheduler");

    status = sched->StreamOpen(5, ioscheduler::kDefaultPri);
    ASSERT_EQ(status, ZX_OK, "Failed to open stream");

    status = sched->StreamClose(5);
    ASSERT_EQ(status, ZX_OK, "Failed to close stream");

    sched->Shutdown();
    sched.release();
    END_TEST;
}

static bool iosched_test_serve() {
    BEGIN_TEST;

    IoSchedulerUniquePtr sched;
    zx_status_t status = IoScheduler::Create(&callbacks, &sched);
    ASSERT_EQ(status, ZX_OK, "Failed to create scheduler");

    status = sched->Init();
    ASSERT_EQ(status, ZX_OK, "Failed to init scheduler");

    status = sched->StreamOpen(5, ioscheduler::kDefaultPri);
    ASSERT_EQ(status, ZX_OK, "Failed to open stream");

    status = sched->Serve();
    ASSERT_EQ(status, ZX_OK, "Failed to being service");

    status = sched->StreamClose(5);
    ASSERT_EQ(status, ZX_OK, "Failed to close stream");

    sched->Shutdown();
    sched.release();
    END_TEST;
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
