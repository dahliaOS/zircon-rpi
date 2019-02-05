// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Temporary for debugging.
#include <stdio.h>

#include <ddk/driver.h>
#include <ddk/platform-defs.h>

#include <zircon/driver/binding.h>
#include <zircon/types.h>

extern zx_status_t hisi_pcie_bind(void* ctx, zx_device_t* device) {
	printf("hisi_pcie_bind\n");
	return ZX_OK;
}

static zx_driver_ops_t hisi_pcie_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = hisi_pcie_bind;
    return ops;
}();

// clang-format off
// Bind to ANY Amlogic SoC with a DWC PCIe controller.
ZIRCON_DRIVER_BEGIN(hisi_pcie, hisi_pcie_driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_96BOARDS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_DW_PCIE),
ZIRCON_DRIVER_END(hisi_pcie)