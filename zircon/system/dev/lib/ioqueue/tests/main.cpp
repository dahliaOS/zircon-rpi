#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>
#include <ioqueue/queue.h>
#include <zircon/listnode.h>

struct TestOp {
    IoOp op;
    list_node_t node;
    uint32_t id;
    bool issued;
    bool released;
};

class IoQueueTest {
public:
    IoQueueTest();

    void Enqueue(TestOp* top);
    void SetQueue(IoQueue* q) { q_ = q; }
    IoQueue* GetQueue() { return q_; }
    void CloseInput(bool wait);
    void GetCounts(uint32_t counts[3]);

    // Callbacks
    static zx_status_t cb_acquire(void* context, IoOp** op_list, size_t* op_count, bool wait) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        return test->AcquireOps(op_list, op_count, wait);
    }

    static zx_status_t cb_issue(void* context, IoOp* op) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        return test->IssueOp(op);
    }

    static void cb_release(void* context, IoOp* op) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        test->ReleaseOp(op);
    }

    static void cb_cancel_acquire(void* context) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        test->CancelAcquire();
    }

    static void cb_fatal(void* context) {
        IoQueueTest* test = static_cast<IoQueueTest*>(context);
        test->Fatal();
    }

private:
    zx_status_t AcquireOps(IoOp** op_list, size_t* op_count, bool wait);
    void CancelAcquire();
    zx_status_t IssueOp(IoOp* op);
    void ReleaseOp(IoOp* op);
    void Fatal();

    IoQueue* q_ = nullptr;

    fbl::Mutex lock_;
    bool closed_ = false;
    uint32_t enqueued_count_ = 0;
    uint32_t issued_count_ = 0;
    uint32_t released_count_ = 0;
    list_node_t in_list_;
    fbl::ConditionVariable in_avail_;
    fbl::ConditionVariable released_all_;
};

IoQueueTest::IoQueueTest() {
    list_initialize(&in_list_);
}

void IoQueueTest::Enqueue(TestOp* top) {
    fbl::AutoLock lock(&lock_);
    list_add_tail(&in_list_, &top->node);
    in_avail_.Signal();
}

void IoQueueTest::GetCounts(uint32_t count[3]) {
    fbl::AutoLock lock(&lock_);
    count[0] = enqueued_count_;
    count[1] = issued_count_;
    count[2] = released_count_;
}

void IoQueueTest::CloseInput(bool wait) {
    printf("%s\n", __FUNCTION__);
    if (wait) {
        fbl::AutoLock lock(&lock_);
        if (!list_is_empty(&in_list_)) {
            printf("%s list is not empty\n", __FUNCTION__);
            released_all_.Wait(&lock_);
            assert(list_is_empty(&in_list_));
            printf("%s emptied\n", __FUNCTION__);
        }
    }
    CancelAcquire();
}

zx_status_t IoQueueTest::AcquireOps(IoOp** op_list, size_t* op_count, bool wait) {
    fbl::AutoLock lock(&lock_);
    printf("cb: acquire\n");
    if (closed_) {
        printf("cb:   closed\n");
        return ZX_ERR_CANCELED;   // Input source closed.
    }
    if (list_is_empty(&in_list_)) {
        if (!wait) {
            return ZX_ERR_SHOULD_WAIT;
        }
        in_avail_.Wait(&lock_);
        if (closed_) {
            return ZX_ERR_CANCELED;
        }
    }
    size_t i, max_ops = *op_count;
    for (i = 0; i < max_ops; i++) {
        list_node_t* node = list_remove_head(&in_list_);
        if (node == nullptr) {
            break;
        }
        TestOp* top = containerof(node, TestOp, node);
        op_list[i] = &top->op;
        printf("cb: acquire %u:%u\n", top->op.stream_id, top->id);
        enqueued_count_++;
    }
    *op_count = i;
    if (list_is_empty(&in_list_)) {
        released_all_.Broadcast();
    }
    return ZX_OK;
}

zx_status_t IoQueueTest::IssueOp(IoOp* op) {
    printf("cb: issue %p\n", op);
    TestOp* top = containerof(op, TestOp, op);
    printf("cb: issue %u:%u\n", op->stream_id, top->id);
    assert(top->issued == false);
    top->issued = true;
    op->result = ZX_OK;
    fbl::AutoLock lock(&lock_);
    issued_count_++;
    return ZX_OK;
}

void IoQueueTest::ReleaseOp(IoOp* op) {
    printf("cb: release %p\n", op);
    TestOp* top = containerof(op, TestOp, op);
    printf("cb: release %u:%u\n", op->stream_id, top->id);
    assert(top->released == false);
    top->released = true;
    fbl::AutoLock lock(&lock_);
    released_count_++;
}

void IoQueueTest::CancelAcquire() {
    printf("cb: cancel_acquire\n");
    fbl::AutoLock lock(&lock_);
    closed_ = true;
    in_avail_.Broadcast();
}

void IoQueueTest::Fatal() {
    printf("cb: FATAL\n");
    assert(false);
}

IoQueueCallbacks cb = {
    .context = NULL,
    .acquire = IoQueueTest::cb_acquire,
    .issue = IoQueueTest::cb_issue,
    .release = IoQueueTest::cb_release,
    .cancel_acquire = IoQueueTest::cb_cancel_acquire,
    .fatal = IoQueueTest::cb_fatal,
};

uint32_t num_workers;


void op_test(IoQueueTest* test, int depth) {
    printf("%s\n", __FUNCTION__);
    const size_t num_ops = 10;
    TestOp tops[num_ops];
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

    // sleep(2);

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
    assert(count[0] == count[1]); // Enqueued == issued.
    assert(count[1] == count[2]); // Issued == Released.
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
    // const int max_depth = 3;
    num_workers = 1;
    printf("num workers = %u\n", num_workers);
    create_test(3);
    // for (int i = 0; i <= max_depth; i++) {
    //     create_test(i);
    // }

    // num_workers = 2;
    // printf("\nnum workers = %u\n", num_workers);
    // for (int i = 0; i <= max_depth; i++) {
    //     create_test(i);
    // }
}

int main(int argc, char* argv[]) {
    printf("IO Queue test\n");
    while(1) {
        do_tests();
    }
    return 0;
}
