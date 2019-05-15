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
#include <fbl/unique_ptr.h>

#include "vim3l.h"

namespace board_vim3l {

zx_status_t Vim3L::Create(void* ctx, zx_device_t* parent) {
    pbus_protocol_t pbus;

    auto status = device_get_protocol(parent, ZX_PROTOCOL_PBUS, &pbus);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: device_get_protocol failed %d\n", __func__, status);
        return status;
    }

    pdev_board_info_t board_info;
    status = pbus_get_board_info(&pbus, &board_info);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: GetBoardInfo failed %d\n", __func__, status);
        return status;
    }

    fbl::AllocChecker ac;
    auto board = fbl::make_unique_checked<Vim3L>(&ac, parent, &pbus, &board_info);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    status = board->DdkAdd("vim3l", DEVICE_ADD_NON_BINDABLE);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DdkAdd failed %d\n", __func__, status);
        return status;
    }

    status = board->Start();
    if (status == ZX_OK) {
        // devmgr is now in charge of the device.
        __UNUSED auto* dummy = board.release();
    }
    return status;
}

int Vim3L::Thread() {
    // Initialize drivers here.

    return 0;
}

zx_status_t Vim3L::Start() {
    auto cb = [](void* arg) -> int { return reinterpret_cast<Vim3L*>(arg)->Thread(); };
    auto rc = thrd_create_with_name(&thread_, cb, this, "vim3l-start-thread");
    if (rc != thrd_success) {
        return ZX_ERR_INTERNAL;
    }
    return ZX_OK;
}

void Vim3L::DdkRelease() {
    delete this;
}

static zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops;
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = Vim3L::Create;
    return ops;
}();

} // namespace board_vim3l

ZIRCON_DRIVER_BEGIN(vim3l, board_vim3l::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PBUS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_KHADAS),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_VIM3L),
ZIRCON_DRIVER_END(vim3l)
