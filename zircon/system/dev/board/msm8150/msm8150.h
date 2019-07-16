// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <threads.h>

#include <ddktl/device.h>
#include <ddktl/protocol/platform/bus.h>

namespace board_msm8150 {

class Msm8150;
using Msm8150Type = ddk::Device<Msm8150>;

// This is the main class for the platform bus driver.
class Msm8150 : public Msm8150Type {
public:
    explicit Msm8150(zx_device_t* parent, pbus_protocol_t* pbus)
        : Msm8150Type(parent), pbus_(pbus) {}

    static zx_status_t Create(void* ctx, zx_device_t* parent);

    // Device protocol implementation.
    void DdkRelease();

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(Msm8150);

    zx_status_t Start();
    int Thread();
/*
    zx_status_t GpioInit();
    zx_status_t PilInit();
    zx_status_t PowerInit();
    zx_status_t Sdc1Init();
    zx_status_t ClockInit();
*/

    ddk::PBusProtocolClient pbus_;
    thrd_t thread_;
};

} // namespace board_msm8150
