#include <assert.h>
#include <stdio.h>

#include <ioqueue/queue.h>

zx_status_t cb_acquire(void* context, io_op_t** op_list, size_t* op_count, bool wait) {
    printf("%s\n", __FUNCTION__);
    return ZX_ERR_CANCELED;
}

zx_status_t cb_issue(void* context, io_op_t* op) {
    printf("%s\n", __FUNCTION__);
    op->result = ZX_OK;
    return ZX_OK;
}

void cb_release(void* context, io_op_t* op) {
    printf("%s\n", __FUNCTION__);
}

void cb_cancel_acquire(void* context) {
    printf("%s\n", __FUNCTION__);
}

void cb_fatal(void* context) {
    printf("%s\n", __FUNCTION__);
}

IoQueueCallbacks cb = {
    .context = NULL,
    .acquire = cb_acquire,
    .issue = cb_issue,
    .release = cb_release,
    .cancel_acquire = cb_cancel_acquire,
    .fatal = cb_fatal,
};

uint32_t num_workers = 1;

void op_test(IoQueue* q, int depth) {


}

void serve_test(IoQueue* q, int depth) {
    printf("%s\n", __FUNCTION__);

    // printf("%s:%u\n", __FUNCTION__, __LINE__);
    zx_status_t status = IoQueueServe(q, num_workers);
    assert(status == ZX_OK);

    if (depth) {
        op_test(q, depth);
    }

    IoQueueShutdown(q);
    printf("%s done\n", __FUNCTION__);
}

void open_test(IoQueue* q, int depth) {
    printf("%s\n", __FUNCTION__);

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
    // Failure expected
    assert(status == ZX_ERR_INVALID_ARGS);

    // valid streams are 0, 1, 2, 4
    if (depth) {
        serve_test(q, depth - 1);
    }

    printf("%s done\n", __FUNCTION__);
}

void create_test(int depth) {
    printf("%s\n", __FUNCTION__);
    IoQueue* q;

    zx_status_t status = IoQueueCreate(&cb, &q);
    assert(status == ZX_OK);

    if (depth) {
        open_test(q, depth - 1);
    }

    IoQueueDestroy(q);
    printf("%s done\n\n", __FUNCTION__);
}

void do_tests() {
    create_test(0);
    create_test(1);
    create_test(3);
}

int main(int argc, char* argv[]) {
    printf("IO Queue test\n");
    do_tests();
    return 0;
}
