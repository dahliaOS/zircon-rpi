// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <limits.h>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/platform-defs.h>
#include <fbl/algorithm.h>
#include <hwreg/bitfields.h>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>
#include <soc/msm8x53/msm8x53-hw.h>

#include "msm8x53.h"

namespace board_msm8x53 {

zx_status_t Msm8x53::UsbInit() {

    const pbus_mmio_t usb_mmios[] = {
        {
            .base = 0x07000000,
            .length = 0x100000,
        },
    };

    const pbus_irq_t usb_irqs[] = {
        {
            .irq = 140 + 32,
            .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
        },
    };

    const pbus_bti_t usb_btis[] = {
        {
            .iommu_index = 0,
            .bti_id = 1,
        },
    };

    pbus_dev_t usb_dev = {};
    usb_dev.name = "dwc3";
    usb_dev.vid = PDEV_VID_GENERIC;
    usb_dev.pid = PDEV_PID_GENERIC;
    usb_dev.did = PDEV_DID_USB_DWC3;
    usb_dev.mmio_list = usb_mmios;
    usb_dev.mmio_count = countof(usb_mmios);
    usb_dev.irq_list = usb_irqs;
    usb_dev.irq_count = countof(usb_irqs);
    usb_dev.bti_list = usb_btis;
    usb_dev.bti_count = countof(usb_btis);

    auto status = pbus_.DeviceAdd(&usb_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DeviceAdd failed %d\n", __func__, status);
        return status;
    }

    return ZX_OK;
}

} // namespace board_msm8x53
