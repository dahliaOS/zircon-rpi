// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <utility>

#include <ddk/debug.h>

#include "server-manager.h"

constexpr uint32_t kDefaultStreamPriority = 0;

ServerManager::ServerManager() = default;

ServerManager::~ServerManager() {
    Shutdown();
}

zx_status_t ServerManager::Start(ddk::BlockProtocolClient* protocol, zx::fifo* out_fifo) {
    // printf("%s:%u\n", __FUNCTION__, __LINE__);
    ServerManagerState state = state_.load();
    if (state == SM_STATE_SERVING) {
        return ZX_ERR_ALREADY_BOUND;
    } else if (state == SM_STATE_EXITED) {
        Shutdown();
    }
    ZX_DEBUG_ASSERT(server_ == nullptr);

    std::unique_ptr<BlockServer> server;
    fzl::fifo<block_fifo_request_t, block_fifo_response_t> fifo;
    zx_status_t status = BlockServer::Create(this, protocol, &fifo, &server);
    if (status != ZX_OK) {
        return status;
    }

    IoQueue* q = nullptr;
    if ((status = IoQueueCreate(server->GetOps(), &q)) != ZX_OK) {
        printf("Failed to allocate queue\n");
        return ZX_ERR_NO_MEMORY;
    }
    IoQueueUniquePtr queue(q);

    for (uint32_t i = 0; i <= MAX_TXN_GROUP_COUNT; i++) {
        if ((status = IoQueueOpenStream(queue.get(), kDefaultStreamPriority, i)) != ZX_OK) {
            printf("Failed to open stream\n");
            return status;
        }
    }

    server_ = std::move(server);
    queue_ = std::move(queue);
    state_.store(SM_STATE_SERVING);
    status = IoQueueServe(queue_.get(), 1);
    if (status != ZX_OK) {
        printf("Serve returned failure\n");
        Shutdown();
        return status;
    }
    *out_fifo = zx::fifo(fifo.release());
    return ZX_OK;
}

void ServerManager::Shutdown() {
    // printf("%s:%u\n", __FUNCTION__, __LINE__);
    if (state_.load() == SM_STATE_SHUTDOWN) {
        return;
    }
    IoQueueShutdown(queue_.get());
    server_->Shutdown();
    server_.reset();
    queue_.reset();
    state_.store(SM_STATE_SHUTDOWN);
}

zx_status_t ServerManager::AttachVmo(zx::vmo vmo, vmoid_t* out_vmoid) {
    // printf("%s:%u\n", __FUNCTION__, __LINE__);
    if (state_.load() != SM_STATE_SERVING) {
        return ZX_ERR_BAD_STATE;
    }
    return server_->AttachVmo(std::move(vmo), out_vmoid);
}

void ServerManager::AsyncClientExited() {
    state_.store(SM_STATE_EXITED);
}
