// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <new>
#include <utility>

#include <ddk/protocol/block.h>
#include <ddktl/device.h>
#include <ddktl/protocol/block.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/mutex.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <fbl/unique_ptr.h>
#include <lib/fzl/fifo.h>
#include <lib/sync/completion.h>
#include <lib/zx/vmo.h>
#include <zircon/device/block.h>
#include <zircon/thread_annotations.h>
#include <zircon/types.h>

#include "io-queue.h"
#include "txn-group.h"

class ServerManager;
using io_op_t = ioqueue::io_op_t;

// Represents the mapping of "vmoid --> VMO"
class IoBuffer : public fbl::WAVLTreeContainable<fbl::RefPtr<IoBuffer>>,
                 public fbl::RefCounted<IoBuffer> {
public:
    vmoid_t GetKey() const { return vmoid_; }

    // TODO(smklein): This function is currently labelled 'hack' since we have
    // no way to ensure that the size of the VMO won't change in between
    // checking it and using it.  This will require a mechanism to "pin" VMO pages.
    // The units of length and vmo_offset is bytes.
    zx_status_t ValidateVmoHack(uint64_t length, uint64_t vmo_offset);

    zx_handle_t vmo() const { return io_vmo_.get(); }

    IoBuffer(zx::vmo vmo, vmoid_t vmoid);
    ~IoBuffer();

private:
    friend struct TypeWAVLTraits;
    DISALLOW_COPY_ASSIGN_AND_MOVE(IoBuffer);

    const zx::vmo io_vmo_;
    const vmoid_t vmoid_;
};

class BlockServer;
struct BlockMessage;

// All the C++ bits of a block message. This allows the block server to utilize
// C++ libraries while also using "block_op_t"s, which may require extra space.
struct BlockMessageHeader {
    BlockMessageHeader(fbl::RefPtr<IoBuffer> iobuf, BlockServer* server,
                       block_fifo_request_t* request) : iobuf_(iobuf), server_(server),
                       reqid_(request->reqid), group_(request->group) {
        iop_.flags = 0;
        iop_.result = ZX_OK;
        iop_.sid = request->group;
    }

    fbl::DoublyLinkedListNodeState<BlockMessage*> dll_node_state_;
    fbl::RefPtr<IoBuffer> iobuf_;
    BlockServer* server_;
    reqid_t reqid_;
    groupid_t group_;
    io_op_t iop_;
};

// A single unit of work transmitted to the underlying block layer.
struct BlockMessage {
    static BlockMessage* FromIoOp(io_op_t* iop) {
        return containerof(iop, BlockMessage, header.iop_);
    }

    BlockMessageHeader header;
    block_op_t bop;
    // + Extra space for underlying block_op
};

// Since the linked list state (necessary to queue up block messages) is based
// in C++ code, but may need to reference the "block_op_t" object, it uses
// a custom type trait.
struct DoublyLinkedListTraits {
    static fbl::DoublyLinkedListNodeState<BlockMessage*>& node_state(BlockMessage& obj) {
        return obj.header.dll_node_state_;
    }
};

using BlockMsgQueue = fbl::DoublyLinkedList<BlockMessage*, DoublyLinkedListTraits>;

// C++ safe wrapper around BlockMessage.
//
// It's difficult to allocate a dynamic-length "block_op" as requested by the
// underlying driver while maintaining valid object construction & destruction;
// this class attempts to hide those details.
class BlockMessageWrapper {
public:
    DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BlockMessageWrapper);

    static zx_status_t Create(size_t block_op_size, fbl::RefPtr<IoBuffer> iobuf,
                              BlockServer* server, block_fifo_request_t* request,
                              BlockMessageWrapper* out);

    bool valid() { return message_ != nullptr; }

    void reset(BlockMessage* message = nullptr) {
        if (message_) {
            message_->header.~BlockMessageHeader();
            free(message_);
        }
        message_ = message;
    }

    BlockMessage* release() {
        auto message = message_;
        message_ = nullptr;
        return message;
    }
    BlockMessageHeader* header() { return &message_->header; }
    block_op_t* bop() { return &message_->bop; }

    BlockMessageWrapper(BlockMessage* message) : message_(message) {}
    BlockMessageWrapper() : message_(nullptr) {}
    BlockMessageWrapper& operator=(BlockMessageWrapper&& o) {
        reset(o.release());
        return *this;
    }

    ~BlockMessageWrapper() {
        reset();
    }

private:
    BlockMessage* message_;
};

class BlockServer {
public:
    // Creates a new BlockServer.
    static zx_status_t Create(ServerManager* manager, ddk::BlockProtocolClient* bp,
                              fzl::fifo<block_fifo_request_t, block_fifo_response_t>* fifo_out,
                              fbl::unique_ptr<BlockServer>* out);

    // Starts the BlockServer using the current thread
    zx_status_t AttachVmo(zx::vmo vmo, vmoid_t* out) TA_EXCL(server_lock_);
    ioqueue::QueueOps* GetOps() { return &ops_; }

    // Updates the total number of pending txns, possibly signals
    // the queue-draining thread to wake up if they are waiting
    // for all pending operations to complete.
    //
    // Should only be called for transactions which have been placed
    // on (and removed from) in_queue_.
    void TxnEnd();

    // Wrapper around "Completed Transaction", as a convenience
    // both both one-shot and group-based transactions.
    //
    // (If appropriate) tells the client that their operation is done.
    void TxnComplete(zx_status_t status, reqid_t reqid, groupid_t group);

    void AsyncBlockComplete(BlockMessage* msg, zx_status_t status);

    void Shutdown();
    ~BlockServer();
private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(BlockServer);
    BlockServer(ServerManager* manager, ddk::BlockProtocolClient* bp);

    // Helper for processing a single message read from the FIFO.
    zx_status_t ProcessRequest(block_fifo_request_t* request);
    zx_status_t ProcessReadWriteRequest(block_fifo_request_t* request) TA_EXCL(server_lock_);
    zx_status_t ProcessCloseVmoRequest(block_fifo_request_t* request) TA_EXCL(server_lock_);
    zx_status_t ProcessFlushRequest(block_fifo_request_t* request);

    // Helper for the server to react to a signal that a barrier
    // operation has completed. Unsets the local "waiting for barrier"
    // signal, and enqueues any further operations that might be
    // pending.
    void BarrierComplete();

    // Functions that read from the fifo and invoke the queue drainer.
    // Should not be invoked concurrently.
    zx_status_t Read(block_fifo_request_t* requests, size_t max, size_t* actual);
    size_t FillFromIntakeQueue(io_op_t** op_list, size_t max_ops);

    zx_status_t FindVmoIDLocked(vmoid_t* out) TA_REQ(server_lock_);

    // Called indirectly by Queue ops.
    zx_status_t Intake(io_op_t** op_list, size_t* op_count, bool wait);
    zx_status_t Service(io_op_t* op);
    void SignalFifoCancel();

    // Queue ops
    static zx_status_t OpAcquire(void* context, io_op_t** op_list, size_t* op_count, bool wait);
    static zx_status_t OpIssue(void* context, io_op_t* op);
    static void OpRelease(void* context, io_op_t* op);
    static void OpCancelAcquire(void* context);
    static void FatalFromQueue(void* context);


    fzl::fifo<block_fifo_response_t, block_fifo_request_t> fifo_;
    block_info_t info_;
    ddk::BlockProtocolClient* bp_;
    size_t block_op_size_;

    // BARRIER_AFTER is implemented by sticking "BARRIER_BEFORE" on the
    // next operation that arrives.
    // bool deferred_barrier_before_ = false;
    BlockMsgQueue intake_queue_;
    std::atomic<size_t> pending_count_;
    std::atomic<bool> barrier_in_progress_;
    TransactionGroup groups_[MAX_TXN_GROUP_COUNT];

    fbl::Mutex server_lock_;
    fbl::WAVLTree<vmoid_t, fbl::RefPtr<IoBuffer>> tree_ TA_GUARDED(server_lock_);
    vmoid_t last_id_ TA_GUARDED(server_lock_);

    ServerManager* manager_;
    ioqueue::QueueOps ops_{};
};
