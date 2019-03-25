// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <stdint.h>

#include <fbl/function.h>
#include <zircon/types.h>

namespace ioscheduler {

// Reordering rules for the scheduler.
// Allow reordering of Read class operations with respect to each other.
constexpr uint32_t kSchedOptReorderReads  = (1u << 0);

// Allow reordering of Write class operations with respect to each other.
constexpr uint32_t kSchedOptReorderWrites = (1u << 1);

// Allow reordering of Read class operations ahead of Write class operations.
constexpr uint32_t kSchedOptReorderReadsAheadOfWrites = (1u << 2);

// Allow reordering of Write class operations ahead of Read class operations.
constexpr uint32_t kSchedOptReorderWritesAheadOfReads = (1u << 3);

// Disallow any reordering.
constexpr uint32_t kSchedOptStrictlyOrdered = 0;

// Allow all reordering options.
constexpr uint32_t kSchedOptFullyOutOfOrder = (kSchedOptReorderReads |
                                               kSchedOptReorderWrites |
                                               kSchedOptReorderReadsAheadOfWrites |
                                               kSchedOptReorderWritesAheadOfReads);

// Maximum priority for a stream.
constexpr uint32_t kMaxPri = 31;

// Suggested default priority for a stream.
constexpr uint32_t kDefaultPri = 8;

// Operation classes.
// These are used to determine respective ordering restrictions of the ops in a stream.
enum class OpClass : uint32_t {
    // Operations that can optionally be reordered.

    kOpClassUnknown = 0, // Always reordered.
    kOpClassRead    = 1, // Read ordering.
    kOpClassWrite   = 2, // Write order.
    kOpClassDiscard = 3, // Write order.
    kOpClassRename  = 4, // Read and Write order.
    kOpClassSync    = 5, // Write order.
    kOpClassCommand = 6, // Read and Write order.

    // Operations that cannot be reordered.

    kOpClassOrderedUnknown = 32, // Always ordered.

    // Barrier operations.

    // Prevent reads from being reordered ahead of this barrier op. No read
    // after this barrier can be issued until this operation has completed.
    kOpClassReadBarrier          = 64,

    // Prevent writes from being reordered after this barrier op. This
    // operation completes after all previous writes in the stream have been
    // issued.
    kOpClassWriteBarrier         = 65,

    // Prevent writes from being reordered after this barrier op. This
    // instruction completes after all previous writes in the stream have been
    // completed.
    kOpClassWriteCompleteBarrier = 66,

    // Combined effects of kOpClassReadBarrier and kOpClassWriteBarrier.
    kOpClassFullBarrier          = 67,

    // Combined effects of kOpClassReadBarrier and kOpClassWriteCompleteBarrier.
    kOpClassFullCompleteBarrier  = 68,

};


constexpr uint32_t kOpFlagComplete =    (1u << 0);
constexpr uint32_t kOpFlagGroupLeader = (1u << 8);

// Reserved 64-bit words for internal use.
constexpr size_t kOpReservedQuads = 12;

struct SchedOp {
    uint32_t op_class;      // Type of operation.
    uint32_t flags;	    // Flags. Should be zero.
    uint32_t group_id;      // Group of operations.
    uint32_t group_members; // Number of members in the group.
    uint32_t _unused;       // Reserved, do not use.
    zx_status_t result;     // Status code of the released operation.
    void* cookie;           // User-defined per-op cookie.
    uint64_t _reserved[kOpReservedQuads]; // Reserved, do not use.
};

// Callback interface from Scheduler to client. Callbacks are made from within
// the Scheduler library to the client implementation. All callbacks are made
// with no locks held and are allowed to block.
struct SchedulerCallbacks {
    // Client-defined opaque pointer returned with every callback.
    void* context;

    // CanReorder
    //   Compare if ops can be reordered with respect to each other. This
    // function is called for every pair of ops whose position in
    // the stream is being considered for reorder relative to each other.
    // Returns:
    //   true if it is safe to reorder |second| ahead of |first|.
    //   false otherwise.
    fbl::Function<bool(void* context,
                       SchedOp* first,
                       SchedOp* second)> CanReorder;

    // Acquire
    //   Read zero or more ops from the client for intake into the
    // Scheduler.
    // Args:
    //   context - the context field from the SchedulerCallbacks struct.
    //   sop_list - an empty array of op pointers to be filled.
    //   list_count - number of entries in sop_list
    //   actual_count - the number of entries filled in sop_list.
    //   wait - block until data is available if true.
    // Returns:
    //   ZX_OK if one or more ops have been added to the list.
    //   ZX_ERR_CANCELED if op source has been closed.
    //   ZX_ERR_SHOULD_WAIT if ops are currently unavailable and |wait| is
    //     false.
    fbl::Function<zx_status_t(void* context,
                              SchedOp** sop_list,
                              size_t list_count,
                              size_t* actual_count,
                              bool wait)> Acquire;

    // Issue
    //   Deliver an op to the IO hardware for immediate execution. This
    // function may block until the op is completed. If it does not block,
    // it should return ZX_ERR_ASYNC.
    // Args:
    //   context - the context field from the SchedulerCallbacks struct.
    //   sop - op to be completed.
    // Returns:
    //   ZX_OK if the op has been completed synchronously or it has failed to
    // issue due to bad parameters in the operation. The callee should update
    // the op’s result field to reflect the success or failure status of the
    // op.
    //   ZX_ERR_ASYNC if the op has been issued for asynchronous completion.
    // Notification of completion should be delivered via the Scheduler’s
    // AsyncComplete() API.
    //   Other error status describing the internal failure that has caused
    // the issue to fail.
    fbl::Function<zx_status_t(void* context, SchedOp* sop)> Issue;

    // Release
    //   Yield ownership of the operation. The completion status of the op
    // is available in its |result| field. Once released, the Scheduler
    // maintains no references to the op and it can be safely deallocated or
    // reused.
    // Args:
    //   context - the context field from the SchedulerCallbacks struct.
    //   sop - op to be released.
    fbl::Function<void(void* context, SchedOp* sop)> Release;

    // CancelAcquire
    //   Cancels any pending blocking calls to Acquire. No further reading of
    // ops should be done. Blocked Acquire callers and any subsequent Acquire
    // calls should return ZX_ERR_CANCELED.
    fbl::Function<void(void* context)> CancelAcquire;

    // Fatal
    //   The Scheduler has encountered a fatal asynchronous error. All pending
    // ops have been aborted. The Scheduler should be shut down and destroyed.
    // The shutdown should be performed from a different context than that of
    // the Fatal() call or else it may deadlock.
    fbl::Function<void(void* context)> Fatal;
};

// Opaque IO Scheduler class.
class Scheduler;

// Synchronous Scheduler interface.

// Allocate a new Scheduler object.
zx_status_t SchedulerCreate(Scheduler** out);

// Initialize a Scheduler object to usable state. Initialize must be called on
// a newly created Scheduler object or Scheduler that has been shut down
// before it can be used.
zx_status_t SchedulerInit(Scheduler* scheduler, SchedulerCallbacks* cb,
                          uint32_t options);

// Open a new stream with the requested ID and priority. It is safe to invoke
// this function from a Scheduler callback context, except from Fatal().
// |id| may not be that of a currently open stream.
// |priority| must be in the inclusive range 0 to kMaxPri.
zx_status_t SchedulerStreamOpen(Scheduler* scheduler, uint32_t id,
                                uint32_t priority);

// Close an open stream. All ops in the stream will be issued before the stream
// is closed. New incoming ops to the closed stream will be released with
// an error.
zx_status_t SchedulerStreamClose(Scheduler* scheduler, uint32_t id);

// Begin scheduler service. This creates the worker threads that will invoke
// the callbacks in SchedulerCallbacks.
zx_status_t SchedulerServe(Scheduler* scheduler);

// End scheduler service. This function blocks until all outstanding ops in
// all streams are completed. Shutdown should not be invoked from a callback
// function. To reuse the scheduler, call SchedulerInit() again.
void SchedulerShutdown(Scheduler* scheduler);

// Delete the Scheduler object. This function is compatible with a
// standard deleter.
void SchedulerDestroy(Scheduler* scheduler);


// Asynchronous Scheduler interface.

// Asynchronous completion. When an issued operation has completed
// asynchronously, this function should be called. The status of the operation
// should be set in |sop|’s result field. This function is non-blocking and
// safe to call from an interrupt handler context.
void AsyncComplete(Scheduler* scheduler, SchedOp* sop);

// Unique_ptr wrapper for class Scheduler.

class SchedulerUniquePtr : public std::unique_ptr<Scheduler, decltype(&SchedulerDestroy)> {
public:
    SchedulerUniquePtr(Scheduler* sched = nullptr) :
        std::unique_ptr<Scheduler, decltype(&SchedulerDestroy)>(sched, SchedulerDestroy) {
    }
};

} // namespace
