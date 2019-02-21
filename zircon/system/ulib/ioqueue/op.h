// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <ioqueue/queue.h>
#include <zircon/listnode.h>
#include <zircon/types.h>

namespace ioqueue {

// ioqueue::Op is the internal version of the public IoOp. They must match in size.
struct Op {
    // These fields must be the same as IoOp.
    enum IoOpcode opcode;
    uint32_t flags;
    uint32_t stream_id;     // Stream id
    zx_status_t result;
    void* cookie;           // User cookie, do not touch.

    // Private fields marked as reserved in IoOp.
    list_node_t node;       // 2x ptr size
    uintptr_t _unused[6];

    static Op* FromIoOp(IoOp* iop) { return reinterpret_cast<Op*>(iop); }
    static IoOp* ToIoOp(Op* op) { return reinterpret_cast<IoOp*>(op); }
    static IoOp** ToIoOpList(Op** list) { return reinterpret_cast<IoOp**>(list); }
};

static_assert(sizeof(Op) == sizeof(IoOp));

} // namespace
