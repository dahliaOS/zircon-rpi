// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <threads.h>

#include <ddktl/device.h>
#include <ddktl/protocol/platform/bus.h>

namespace board_vim3l {

class Vim3L;
using Vim3LType = ddk::Device<Vim3L>;

// This is the main class for the platform bus driver.
class Vim3L : public Vim3LType {
public:
    explicit Vim3L(zx_device_t* parent, pbus_protocol_t* pbus, pdev_board_info_t* board_info)
        : Vim3LType(parent), pbus_(pbus), board_info_(*board_info) {}

    static zx_status_t Create(void* ctx, zx_device_t* parent);

    // Device protocol implementation.
    void DdkRelease();

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(Vim3L);

    zx_status_t Start();
    int Thread();

    ddk::PBusProtocolClient pbus_;
    pdev_board_info_t board_info_;
    thrd_t thread_;
};

} // namespace board_vim3l
