// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fbl/auto_lock.h>
#include <unittest/unittest.h>

#include "ioqueue-test.h"

namespace tests {

IoQueueCallbacks cb = {
    .context = NULL,
    .acquire = IoQueueTest::cb_acquire,
    .issue = IoQueueTest::cb_issue,
    .release = IoQueueTest::cb_release,
    .cancel_acquire = IoQueueTest::cb_cancel_acquire,
    .fatal = IoQueueTest::cb_fatal,
};

const size_t num_ops = 10;
TestOp tops[num_ops];

static bool ioqueue_create(IoQueueTest* test) {
    IoQueue* q;
    cb.context = static_cast<void*>(test);
    ASSERT_EQ(IoQueueCreate(&cb, &q), ZX_OK, "Failed to create IoQueue");
    test->SetQueue(q);
    return true;
}

static void ioqueue_destroy(IoQueueTest* test) {
    IoQueueDestroy(test->GetQueue());
}

static bool ioqueue_open_streams(IoQueueTest* test) {
    IoQueue* q = test->GetQueue();
    // Open stream 0 at priority 10
    ASSERT_EQ(IoQueueOpenStream(q, 10, 0), ZX_OK, "Failed to open stream");
     // Open stream 2 at priority 12
    ASSERT_EQ(IoQueueOpenStream(q, 12, 2), ZX_OK, "Failed to open stream");
     // Open stream 1 at priority 12
    ASSERT_EQ(IoQueueOpenStream(q, 12, 1), ZX_OK, "Failed to open stream");
    // Open stream 4 at priority 9
    ASSERT_EQ(IoQueueOpenStream(q, 9, 4), ZX_OK, "Failed to open stream");
    // Open stream 2 at priority 18
    // Failure expected, stream already open.
    ASSERT_EQ(IoQueueOpenStream(q, 18, 2), ZX_ERR_ALREADY_EXISTS, "Expected stream already open");
    // Open stream 7 at invalid priority.
    ASSERT_EQ(IoQueueOpenStream(q, kIoQueueMaxPri + 5, 7), ZX_ERR_INVALID_ARGS, "Expected invalid priority");
    // valid streams are 0, 1, 2, 4
    return true;
}

static bool ioqueue_close_streams(IoQueueTest* test) {
    printf("closing streams\n");
    IoQueue* q = test->GetQueue();
    ASSERT_EQ(IoQueueCloseStream(q, 0), ZX_OK, "Failed to close stream");
    ASSERT_EQ(IoQueueCloseStream(q, 1), ZX_OK, "Failed to close stream");

    ASSERT_EQ(IoQueueCloseStream(q, 1), ZX_ERR_INVALID_ARGS, "Closed non-existent stream");

    ASSERT_EQ(IoQueueCloseStream(q, 2), ZX_OK, "Failed to close stream");
    ASSERT_EQ(IoQueueCloseStream(q, 4), ZX_OK, "Failed to close stream");
    printf("closing streams done\n");
    return true;
}

static bool ioqueue_serve(IoQueueTest* test) {
    IoQueue* q = test->GetQueue();
    ASSERT_EQ(IoQueueServe(q, test->GetWorkers()), ZX_OK, "Failed to start server");
    return true;
}

static bool ioqueue_shutdown(IoQueueTest* test) {
    IoQueue* q = test->GetQueue();
    IoQueueShutdown(q);
    uint32_t count[3];
    test->GetCounts(count);
    printf("enq = %u, iss = %u, rel = %u\n", count[0], count[1], count[2]);
    ASSERT_EQ(count[0], count[2], "Ops enqueued do not equal ops released");
    return true;
}

static void ioqueue_enqueue(IoQueueTest* test) {
    memset(tops, 0, sizeof(tops));
    tops[0].id = 100;
    tops[0].op.stream_id = 0;
    tops[1].id = 101;
    tops[1].op.stream_id = 0;
    test->Enqueue(&tops[0]);
    test->Enqueue(&tops[1]);
    tops[2].id = 102;
    tops[2].op.stream_id = 2;
    test->Enqueue(&tops[2]);
    tops[3].id = 103;
    tops[3].op.stream_id = 4;
    tops[4].id = 104;
    tops[4].op.stream_id = 0;
    test->Enqueue(&tops[3]);
    test->Enqueue(&tops[4]);
}

static bool ioqueue_wait_for_completion(IoQueueTest* test) {
    test->CloseInput(true);
    uint32_t count[3];
    test->GetCounts(count);
    uint32_t enqueued = count[0];
    for (uint32_t i = 0; i < enqueued; i++) {
        ASSERT_TRUE(tops[i].enqueued, "Op not queued");
        ASSERT_TRUE(tops[i].released, "Op not released");
    }
    return true;
}

static uint32_t use_num_threads = 1;

static bool ioqueue_test_create() {
    BEGIN_TEST;
    IoQueueTest test(use_num_threads);
    ASSERT_TRUE(ioqueue_create(&test), "Queue create failed");
    ioqueue_destroy(&test);
    END_TEST;
}

static bool ioqueue_test_open() {
    BEGIN_TEST;
    IoQueueTest test(use_num_threads);
    ASSERT_TRUE(ioqueue_create(&test), "Queue create failed");
    ioqueue_open_streams(&test);
    ioqueue_close_streams(&test);
    ioqueue_destroy(&test);
    END_TEST;
}

static bool ioqueue_test_serve() {
    BEGIN_TEST;
    IoQueueTest test(use_num_threads);
    ASSERT_TRUE(ioqueue_create(&test), "Queue create failed");
    if (ioqueue_open_streams(&test)) {
        ioqueue_enqueue(&test);
        if (ioqueue_serve(&test)) {
            ioqueue_wait_for_completion(&test);
            ioqueue_shutdown(&test);
        } else {
            ioqueue_close_streams(&test);
        }
    }
    ioqueue_destroy(&test);
    END_TEST;
}

BEGIN_TEST_CASE(ioqueue_tests)
// RUN_TEST(ioqueue_test_create)
// RUN_TEST(ioqueue_test_open)
RUN_TEST(ioqueue_test_serve)
END_TEST_CASE(ioqueue_tests)

} // namespace tests

int main(int argc, char** argv) {
    bool success = unittest_run_all_tests(argc, argv);
    return success ? 0 : -1;
}

