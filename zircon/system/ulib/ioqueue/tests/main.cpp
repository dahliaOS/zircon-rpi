// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fbl/auto_lock.h>

#include "ioqueue-test.h"

IoQueueCallbacks cb = {
    .context = NULL,
    .acquire = IoQueueTest::cb_acquire,
    .issue = IoQueueTest::cb_issue,
    .release = IoQueueTest::cb_release,
    .cancel_acquire = IoQueueTest::cb_cancel_acquire,
    .fatal = IoQueueTest::cb_fatal,
};

uint32_t num_workers;
const size_t num_ops = 10;
TestOp tops[num_ops];

void op_test(IoQueueTest* test, int depth) {
    printf("%s\n", __FUNCTION__);
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

    test->CloseInput(true);
    printf("%s done\n", __FUNCTION__);
}

void serve_test(IoQueueTest* test, int depth) {
    printf("%s\n", __FUNCTION__);
    IoQueue* q = test->GetQueue();

    // printf("%s:%u\n", __FUNCTION__, __LINE__);
    zx_status_t status = IoQueueServe(q, num_workers);
    assert(status == ZX_OK);

    if (depth) {
        op_test(test, depth - 1);
    }
    IoQueueShutdown(q);

    uint32_t count[3];
    test->GetCounts(count);
    printf("enq = %u, iss = %u, rel = %u\n", count[0], count[1], count[2]);
    assert(count[0] == count[2]); // Enqueued == Released.
    printf("%s done\n", __FUNCTION__);
}

void open_test(IoQueueTest* test, int depth) {
    printf("%s\n", __FUNCTION__);
    IoQueue* q = test->GetQueue();
    zx_status_t status = IoQueueOpenStream(q, 10, 0); // Open stream 0 at priority 10
    assert(status == ZX_OK);

    status = IoQueueOpenStream(q, 12, 2); // Open stream 2 at priority 12
    assert(status == ZX_OK);

    status = IoQueueOpenStream(q, 12, 1); // Open stream 1 at priority 12
    assert(status == ZX_OK);

    status = IoQueueOpenStream(q, 9, 4); // Open stream 4 at priority 9
    assert(status == ZX_OK);

    status = IoQueueOpenStream(q, 18, 2); // Open stream 2 at priority 18
    // Failure expected, stream already open.
    assert(status == ZX_ERR_ALREADY_EXISTS);

    status = IoQueueOpenStream(q, kIoQueueMaxPri + 5, 7); // Open stream 7 at invalid priority.
    // Failure expected.
    assert(status == ZX_ERR_INVALID_ARGS);

    // valid streams are 0, 1, 2, 4
    if (depth) {
        serve_test(test, depth - 1);
    }

    printf("%s done\n", __FUNCTION__);
}

void create_test(int depth) {
    printf("%s\n", __FUNCTION__);
    IoQueue* q;

    IoQueueTest test;
    cb.context = static_cast<void*>(&test);

    zx_status_t status = IoQueueCreate(&cb, &q);
    assert(status == ZX_OK);
    test.SetQueue(q);

    if (depth) {
        open_test(&test, depth - 1);
    }

    IoQueueDestroy(q);
    printf("%s done\n\n", __FUNCTION__);
}

void do_tests() {
    const int max_depth = 3;
    num_workers = 1;
    printf("num workers = %u\n", num_workers);
    for (int i = 0; i <= max_depth; i++) {
        create_test(i);
    }

    num_workers = 2;
    printf("\nnum workers = %u\n", num_workers);
    for (int i = 0; i <= max_depth; i++) {
        create_test(i);
    }
}

int main(int argc, char* argv[]) {
    printf("IO Queue test\n");
    while(1) {
        do_tests();
    }
    return 0;
}
