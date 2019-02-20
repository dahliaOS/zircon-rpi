// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <stdint.h>

#include <zircon/types.h>

/*
IO Queue - an IO scheduler.

The IO Queue consists of a queue of operations (ops), an op scheduler, and one or more worker threads that service them. It is designed to service a request source, schedule sequential and concurrent requests based on priority and client capabilities, and retire the requests. The queue is designed to maximize the throughput of request services and concurrency of their execution.

Ops are sorted into streams. A stream is a logical sequence that a client may wish to enforce on operations. A stream has an associated priority level. Streams and their priorities allow a client to balance op distribution. Ops in a streams are NOT strictly ordered. The client must issue barrier ops to enforce ordering. Streams are not ordered with respect to each other.
TODO(sron): add ordering options.

The queue schedules the streams in a priority-based round-robin. One or more worker threads may service the queue, acquiring, issuing, and releasing ops.

The queue core makes the following callbacks to the clients:

    Acquire - fetch one or more ops from the op source.

    Issue - perform the operations described by the op, either in a blocking or non-blocking fashion.

    Release - op is complete and all references from it inside the IO Queue have been removed.

The queue will perform one or more concurrent callbacks based on client-defined parameters.
*/

constexpr uint32_t kIoQueueMaxPri = 31;
constexpr uint32_t kIoQueueDefaultPri = 8;

/*
IoOp is the generic unit of IO operation. This structure is used by the IO Queue to schedule ops. It has no significance outside of this library and should not be generally used. It is expected to be part of an implementation-specific structure that includes the relevant information needed to execute it. The IO Queue does not allocate or free ops. Once an op has been acquired but the IO Queue, it is not safe to free it until it has been released by the IO Queue.

Fields:
opcode - set by client. the logical operation to be performed. The opcode is used by the queue for sorting, consolidation, and execution decisions. This is not strictly required and may be IoOpUnknown, which may be fully reordered except by barriers.
flags - unused, should be zero.
stream_id - set by client. ID of an open stream to which this op should be appended.
result - status of the operation upon release. Initialization not required.
_reserved - private internal fields, do not modify.
*/
struct IoOp {
    uint32_t opcode;
    uint32_t flags;
    uint32_t stream_id;
    zx_status_t result;
    uint64_t _reserved[6];
};

/*
IoQueueCallbacks are made from within the IO Queue to the client implementation. All callbacks are made with no locks held and are allowed to block.

Fields:
context - a client-defined opaque pointer that is returned in the callbacks.
acquire - A function that fills an array of IoOp.

Acquire
--------
Acquire reads zero or more ops and returns pointers to them in op_list.

Args:
    context - the context field from the IoQueueCallbacks struct.
    op_list - an array of op pointers.
    op_count - IN, the maximum number of entries in op_list. OUT, the number of ops copied in.
    wait - block until data is available.

Returns:
    ZX_OK if one or more ops have been added to the list.
    ZX_ERR_CANCELED if the op source has been closed.
    ZX_ERR_SHOULD_WAIT if no ops are available and |wait| is false.

Issue
--------
Performs the work described by the op. The operation may be performed synchronously or may be initiated for asynchronous completion. In the case of an synchronous operation, the op's result field must be updated to reflect the status. Asynchronous operations must call IoQueueAsyncCompleteOp() upon completion.

The result of the operation should be set in the op's result field if it is completed synchronously.

Args:
    context - the context field from the IoQueueCallbacks struct.
    op - operation to execute.

Returns:
    The returned value is the status of the issue call, which is distinct from the status of the op that was passed to it. The latter is returned in the op's result field.
    ZX_OK if the operation has completed or failed.
    ZX_ERR_ASYNC if the operation will complete asynchronously.

Release
--------
Called when an op and all of its dependencies have completed. The status of the operation is available in the op's result field. Once released, the IO Queue maintains no references to the op and it can be safely deallocated or reused.

Args:
    context - the context field from the IoQueueCallbacks struct.
    op - operation to release.

Cancel_Acquire
--------
Called to wake any blocked callers to acquire and to signal that no further reading of ops should be done. Acquire callers should return ZX_ERR_CANCELED. May be invoked multiple times until IoQueueShutdown() is called.

Args:
    context - the context field from the IoQueueCallbacks struct.

Fatal
--------
The IO Queue has encountered a fatal error and cannot continue service. This is a chance for the client to clean up and exit prior to a crash.
*/
struct IoQueueCallbacks {
    // User-provided context structure returned in the below callbacks.
    void* context;
    // Returns true if second op can be reordered ahead of the first one.
    // bool (*can_reorder)(void* context, IoOp* first, IoOp* second);
    // Get ops from source.
    zx_status_t (*acquire)(void* context, IoOp** op_list, size_t* op_count, bool wait);
    // Executes the op. May not be called if dependencies have failed.
    zx_status_t (*issue)(void* context, IoOp* op);
    // An op has completed. Called once for every scheduled op. Queue maintains no references
    // to |op| after this call and it is safe to delete or reuse.
    void (*release)(void* context, IoOp* op);
    // Called during shutdown to interrupt blocked acquire callback.
    // Acquire calls following this should return ZX_ERR_CANCELED.
    void (*cancel_acquire)(void* context);
    // A fatal error has occurred. Queue should be shut down.
    void (*fatal)(void* context);
};

class IoQueue;


// Allocate a new IoQueue object.
zx_status_t IoQueueCreate(const IoQueueCallbacks* cb, IoQueue** q_out);
// Open a stream with priority and ID number.
zx_status_t IoQueueOpenStream(IoQueue* q, uint32_t priority, uint32_t id);
// Close stream. Closing will block until all ops scheduled on stream have completed.
zx_status_t IoQueueCloseStream(IoQueue* q, uint32_t id);
// Begin service, employing |num_workers| worker threads. The minimum is one worker, but at least two are recommended for performance reasons.
zx_status_t IoQueueServe(IoQueue* q, uint32_t num_workers);
// End service. This call will block until all outstanding operations have completed.
void IoQueueShutdown(IoQueue* q);
// Deallocate the queue.
void IoQueueDestroy(IoQueue* q);

// Notify the IO Queue that an asynchronous operation has completed.
// This operation will not block and is safe to call from an interrupt handler. TODO: not true yet.
void IoQueueAsyncCompleteOp(IoQueue* q, IoOp* op);

// A convenience class that provides uinque_ptr functionality and knows how to destroy an IoQueue object.
class IoQueueUniquePtr : public std::unique_ptr<IoQueue, decltype(&IoQueueDestroy)> {
public:
    IoQueueUniquePtr(IoQueue* q = nullptr) :
        std::unique_ptr<IoQueue, decltype(&IoQueueDestroy)>(q, IoQueueDestroy) {
    }
};

