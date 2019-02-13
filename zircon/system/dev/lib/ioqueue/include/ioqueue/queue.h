// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <zircon/listnode.h>
#include <zircon/types.h>

constexpr uint32_t kIoQueueMaxPri = 31;
constexpr uint32_t kIoQueueDefaultPri = 8;

typedef struct {
    list_node_t node;    // Reserved, internal only.
    uint32_t opcode;
    uint32_t flags;
    uint32_t sid;        // Stream id
    zx_status_t result;
} io_op_t;

struct IoQueueCallbacks {
    // User-provided context structure returned in the below callbacks.
    void* context;
    // Returns true if second op can be reordered ahead of the first one.
    // bool (*can_reorder)(struct io_queue* q, io_op_t* first, io_op_t* second);
    // Get ops from source.
    zx_status_t (*acquire)(void* context, io_op_t** op_list, size_t* op_count, bool wait);
    // Executes the op. May not be called if dependencies have failed.
    zx_status_t (*issue)(void* context, io_op_t* op);
    // An op has completed. Called once for every scheduled op. Queue maintains no references
    // to |op| after this call and it is safe to delete or reuse.
    void (*release)(void* context, io_op_t* op);
    // Called during shutdown to interrupt blocked acquire callback.
    // Acquire calls following this should return ZX_ERR_CANCELED.
    void (*cancel_acquire)(void* context);
    // A fatal error has occurred. Queue should be shut down.
    void (*fatal)(void* context);
};

typedef void IoQueue;

zx_status_t IoQueueCreate(const IoQueueCallbacks* cb, IoQueue** q_out);
zx_status_t IoQueueOpenStream(IoQueue* q, uint32_t priority, uint32_t id);
zx_status_t IoQueueCloseStream(IoQueue* q, uint32_t id);
zx_status_t IoQueueServe(IoQueue* q, uint32_t num_workers);
void IoQueueShutdown(IoQueue* q);
void IoQueueDestroy(IoQueue* q);

void IoQueueAsyncCompleteOp(IoQueue* q, io_op_t* op, zx_status_t result);
