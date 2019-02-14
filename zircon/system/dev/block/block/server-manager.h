// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <atomic>
#include <threads.h>

#include <ddktl/protocol/block.h>
#include <lib/zx/fifo.h>
#include <lib/zx/vmo.h>
#include <ioqueue/queue.h>
#include <zircon/types.h>

#include "server.h"

// ServerManager controls the state of a background thread (or threads) servicing Fifo
// requests.
class ServerManager {
public:
    DISALLOW_COPY_ASSIGN_AND_MOVE(ServerManager);

    ServerManager();
    ~ServerManager();

    // Launches the Fifo server in a background thread.
    //
    // Returns an error if the block server cannot be created.
    // Returns an error if the Fifo server is already running.
    zx_status_t Start(ddk::BlockProtocolClient* protocol, zx::fifo* out_fifo);

    // Ensures the FIFO server has terminated.
    //
    // When this function returns, it is guaranteed that the next call to |StartServer()|
    // won't see an already running Fifo server.
    void Shutdown();

    // Attaches a VMO to the currently executing server, if one is running.
    //
    // Returns an error if a server is not currently running.
    zx_status_t AttachVmo(zx::vmo vmo, vmoid_t* out_vmoid);

    inline void AsyncCompleteNotify(IoOp* op) { IoQueueAsyncCompleteOp(queue_.get(), op); }

    // Notification that the client FIFO has been closed.
    void AsyncClientExited();

private:
    enum ServerManagerState {
        // No server is currently executing.
        SM_STATE_SHUTDOWN = 0,
        // The server is executing right now.
        SM_STATE_SERVING,
        // The server has finished executing, and is ready to be joined.
        SM_STATE_EXITED,
    };

    std::atomic<ServerManagerState> state_ = SM_STATE_SHUTDOWN;
    std::unique_ptr<BlockServer> server_ = nullptr;
    IoQueueUniquePtr queue_;
};
