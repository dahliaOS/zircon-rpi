// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>

#include <zircon/types.h>

extern zx_status_t rust_example_bind(void* ctx, zx_device_t* device);

static zx_driver_ops_t rust_example_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = rust_example_bind,
};

// TODO(bwb): consider implementing rust macro
//ZIRCON_DRIVER!(rust_example, rust_example_driver_ops, "zircon", "0.1" [
//    ABORT_IF_NE!(BIND_PROTOCOL, ZX_PROTOCOL_DEV),
//    ...
//])

// clang-format off
ZIRCON_DRIVER_BEGIN(rust_example, rust_example_driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_GPIO_LIGHT),
//  BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_MISC_PARENT),
ZIRCON_DRIVER_END(rust_example)
