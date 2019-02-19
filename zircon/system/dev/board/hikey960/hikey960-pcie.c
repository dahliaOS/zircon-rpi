// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/platform-defs.h>
#include <soc/hi3660/hi3660-hw.h>

#include "hikey960-hw.h"
#include "hikey960.h"

static const pbus_bti_t pci_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = 0,
    },
};

static const pbus_dev_t pcie_dev_children[] = {
    {
        .bti_list = pci_btis,
        .bti_count = countof(pci_btis),
    },
};

static const pbus_mmio_t mmios[] = {
    // DBI.
    {
        .base = 0xf4000000,
        .length = 0x1000,
    },

    // APB.
    {
        .base = 0xff3fe000,
        .length = 0x1000,
    },

    // Phy.
    {
        .base = 0xf3f20000,
        .length = 0x40000,
    },

    // Config.
    {
        .base = 0xf5000000,
        .length = 0x2000,
    },

    // crgctrl.
    {
        .base = 0xfff35000,
        .length = 0x1000,
    },

    // sctrl.
    {
        .base = 0xfff0a000,
        .length = 0x1000,
    },

    // ATU config space.
    {
        .base =   0xf6000000,
        .length = 0x02000000, // 32MiB
    }

};

static const pbus_gpio_t gpios[] = {
    {
        .gpio = GPIO_PCIE_RST,
    },
};

static const pbus_irq_t irqs[] = {
    {
        .irq = IRQ_PCIE_RADM_INTA,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
    {
        .irq = IRQ_PCIE_RADM_INTB,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
    {
        .irq = IRQ_PCIE_RADM_INTC,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
    {
        .irq = IRQ_PCIE_RADM_INTD,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
};

static const pbus_bti_t btis[] = {
    {
        .iommu_index = 0,
        .bti_id = 0,
    },
};

static const pbus_clk_t clks[] = {
    // {  // TODO(gkalsi).
    //     .clk = HI3660_PCIEPHY_REF,
    // },
    {
        .clk = HI3660_CLK_GATE_PCIEAUX,
    },
    {
        .clk = HI3660_PCLK_GATE_PCIE_PHY,
    },
    {
        .clk = HI3660_PCLK_GATE_PCIE_SYS,
    },
    {
        .clk = HI3660_ACLK_GATE_PCIE,
    },

};

const pbus_dev_t hikey_pcie_dev = {
    .name = "hikey-pcie",

    .vid = PDEV_VID_96BOARDS,
    .pid = PDEV_PID_GENERIC,
    .did = PDEV_DID_DW_PCIE,

    .mmio_list = mmios,
    .mmio_count = countof(mmios),

    .irq_list = irqs,
    .irq_count = countof(irqs),

    .gpio_list = gpios,
    .gpio_count = countof(gpios),

    .clk_list = clks,
    .clk_count = countof(clks),

    .child_list = pcie_dev_children,
    .child_count = countof(pcie_dev_children),

    .bti_list = btis,
    .bti_count = countof(btis),
};

zx_status_t hikey960_pcie_init(hikey960_t* hikey) {
    zx_status_t st;

    st = pbus_device_add(&hikey->pbus, &hikey_pcie_dev);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hikey960_add_device could not add hikey_usb_dev: %d\n",
               st);
        return st;
    }

    return ZX_OK;
}