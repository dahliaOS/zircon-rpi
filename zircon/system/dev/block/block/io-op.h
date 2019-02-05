// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <zircon/listnode.h>
#include <zircon/types.h>

namespace ioqueue {

// constexpr uint32_t kOpFlagReadBarrier  = (1u << 0);
// constexpr uint32_t kOpFlagWriteBarrier = (1u << 1);
// constexpr uint32_t kOpFlagReorderBarrier = (1u << 2);
// constexpr uint32_t kOpFlagFullBarrier = kOpFlagReadBarrier | kOpFlagWriteBarrier;


typedef struct {
    list_node_t node;    // Reserved, internal only.
    uint32_t opcode;
    uint32_t flags;
    uint32_t sid;        // Stream id
    zx_status_t result;
} io_op_t;

} // namespace
