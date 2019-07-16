// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/platform/bus.h>

#include <memory>

#include "msm8150.h"

namespace board_msm8150 {

zx_status_t Msm8150::Create(void* ctx, zx_device_t* parent) {
    pbus_protocol_t pbus;

    auto status = device_get_protocol(parent, ZX_PROTOCOL_PBUS, &pbus);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: device_get_protocol failed %d\n", __func__, status);
        return status;
    }

    auto board = std::make_unique<Msm8150>(parent, &pbus);
    status = board->DdkAdd("msm8150", DEVICE_ADD_NON_BINDABLE);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DdkAdd failed %d\n", __func__, status);
        return status;
    }

    // Start up our protocol helpers and platform devices.
    status = board->Start();
    if (status == ZX_OK) {
        // devmgr is now in charge of the device.
        __UNUSED auto* dummy = board.release();
    }
    return status;
}

int Msm8150::Thread() {
/*
    if (GpioInit() != ZX_OK) {
        zxlogf(ERROR, "GpioInit() failed\n");
        return -1;
    }

    if (ClockInit() != ZX_OK) {
        zxlogf(ERROR, "ClockInit failed\n");
        return -1;
    }

    if (PowerInit() != ZX_OK) {
        zxlogf(ERROR, "PowerInit() failed\n");
        return -1;
    }

    if (PilInit() != ZX_OK) {
        zxlogf(ERROR, "PilInit() failed\n");
        return -1;
    }

    if (Sdc1Init() != ZX_OK) {
        zxlogf(ERROR, "Sdc1Init() failed\n");
        return -1;
    }
*/
    return 0;
}

zx_status_t Msm8150::Start() {
    auto cb = [](void* arg) -> int { return reinterpret_cast<Msm8150*>(arg)->Thread(); };
    auto rc = thrd_create_with_name(&thread_, cb, this, "msm8150-start-thread");
    if (rc != thrd_success) {
        return ZX_ERR_INTERNAL;
    }
    return ZX_OK;
}

void Msm8150::DdkRelease() {
    delete this;
}

static constexpr zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = Msm8150::Create;
    return ops;
}();

} // namespace board_msm8150

ZIRCON_DRIVER_BEGIN(msm8150, board_msm8150::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PBUS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_QUALCOMM),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_QUALCOMM_MSM8150),
ZIRCON_DRIVER_END(msm8150)
