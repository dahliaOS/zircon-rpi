// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <ioqueue/queue.h>
#include <zircon/listnode.h>
#include <zircon/types.h>

namespace ioqueue {

// constexpr uint32_t kOpFlagReadBarrier  = (1u << 0);
// constexpr uint32_t kOpFlagWriteBarrier = (1u << 1);
// constexpr uint32_t kOpFlagReorderBarrier = (1u << 2);
// constexpr uint32_t kOpFlagFullBarrier = kOpFlagReadBarrier | kOpFlagWriteBarrier;

// ioqueue::Op is the internal version of the public IoOp. They must match in size.
struct Op {
    // These fields must be the same as IoOp.
    uint32_t opcode;
    uint32_t flags;
    uint32_t stream_id;     // Stream id
    zx_status_t result;

    // Private fields marked as reserved in IoOp.
    list_node_t node;       // 2x ptr size
    uint64_t _unused[4];

    static IoOp* ToIoOp(Op* op) { return reinterpret_cast<IoOp*>(op); }
    static IoOp** ToIoOpList(Op** list) { return reinterpret_cast<IoOp**>(list); }
};

static_assert(sizeof(Op) == sizeof(IoOp));

} // namespace
