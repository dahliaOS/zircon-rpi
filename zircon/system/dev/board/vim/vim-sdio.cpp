// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <fbl/algorithm.h>
#include <hw/reg.h>
#include <soc/aml-a113/a113-hw.h>
#include <soc/aml-common/aml-sd-emmc.h>
#include <soc/aml-s912/s912-gpio.h>
#include <soc/aml-s912/s912-hw.h>
#include <wifi/wifi-config.h>

#include "vim.h"

namespace vim {

static const pbus_mmio_t aml_sd_emmc_mmios[] = {
    {
        .base = 0xD0070000,
        .length = 0x2000,
    }};

static const pbus_irq_t aml_sd_emmc_irqs[] = {
    {
        .irq = 248,
        //c++ initialization error
        .mode = 0,
        //c++ initialization error
    },
};

static const pbus_bti_t aml_sd_emmc_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_SDIO,
    },
};

static const pbus_gpio_t aml_sd_emmc_gpios[] = {
    {
        .gpio = S912_GPIOX(6),
    },
};

static aml_sd_emmc_config_t config = {
    .supports_dma = true,
    .min_freq = 400000,
    .max_freq = 100000000,
};

static const wifi_config_t wifi_config = {
    .oob_irq_mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
};


static const pbus_metadata_t aml_sd_emmc_metadata[] = {
    {
        .type = DEVICE_METADATA_EMMC_CONFIG,
        .data_buffer = &config,
        .data_size = sizeof(config),
    },
    {
        .type = DEVICE_METADATA_WIFI_CONFIG,
        .data_buffer = &wifi_config,
        .data_size = sizeof(wifi_config),
    },
};

static const pbus_dev_t aml_sd_emmc_dev = []() {
    pbus_dev_t dev;

    dev.name = "aml-sdio";
    dev.vid = PDEV_VID_AMLOGIC;
    dev.pid = PDEV_PID_GENERIC;
    dev.did = PDEV_DID_AMLOGIC_SD_EMMC_A;
    dev.mmio_list = aml_sd_emmc_mmios;
    dev.mmio_count = countof(aml_sd_emmc_mmios);
    dev.irq_list = aml_sd_emmc_irqs;
    dev.irq_count = countof(aml_sd_emmc_irqs);
    dev.gpio_list = aml_sd_emmc_gpios;
    dev.gpio_count = countof(aml_sd_emmc_gpios);
    dev.bti_list = aml_sd_emmc_btis;
    dev.bti_count = countof(aml_sd_emmc_btis);
    dev.metadata_list = aml_sd_emmc_metadata;
    dev.metadata_count = countof(aml_sd_emmc_metadata);
    return dev;
}();

zx_status_t Vim::SdioInit() {
    zx_status_t status;

    gpio_impl_.SetAltFunction(S912_WIFI_SDIO_D0, S912_WIFI_SDIO_D0_FN);
    gpio_impl_.SetAltFunction(S912_WIFI_SDIO_D1, S912_WIFI_SDIO_D1_FN);
    gpio_impl_.SetAltFunction(S912_WIFI_SDIO_D2, S912_WIFI_SDIO_D2_FN);
    gpio_impl_.SetAltFunction(S912_WIFI_SDIO_D3, S912_WIFI_SDIO_D3_FN);
    gpio_impl_.SetAltFunction(S912_WIFI_SDIO_CLK, S912_WIFI_SDIO_CLK_FN);
    gpio_impl_.SetAltFunction(S912_WIFI_SDIO_CMD, S912_WIFI_SDIO_CMD_FN);
    gpio_impl_.SetAltFunction(S912_WIFI_SDIO_WAKE_HOST, S912_WIFI_SDIO_WAKE_HOST_FN);

    if ((status = pbus_.DeviceAdd(&aml_sd_emmc_dev)) != ZX_OK) {
        zxlogf(ERROR, "SdioInit could not add aml_sd_emmc_dev: %d\n", status);
        return status;
    }

    // Add a composite device
    const zx_bind_inst_t root_match[] = {
        BI_MATCH(),
    };
    const zx_bind_inst_t sdio_match[]  = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_AMLOGIC),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
        BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_AMLOGIC_SD_EMMC_A),
    };
    const zx_bind_inst_t gpio_impl_match[]  = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_AMLOGIC),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_AMLOGIC_S912),
        BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_AMLOGIC_GPIO),
    };
    const zx_bind_inst_t oob_gpio_match[] = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_GPIO),
        BI_MATCH_IF(EQ, BIND_GPIO_PIN, S912_WIFI_SDIO_WAKE_HOST),
    };
    const zx_bind_inst_t debug_gpio_match[] = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_GPIO),
        BI_MATCH_IF(EQ, BIND_GPIO_PIN, S912_GPIODV(13)),
    };
    device_component_part_t sdio_component[] = {
        { fbl::count_of(root_match), root_match },
        { fbl::count_of(sdio_match), sdio_match },
    };
    device_component_part_t oob_gpio_component[] = {
        { fbl::count_of(root_match), root_match },
        { fbl::count_of(gpio_impl_match), gpio_impl_match },
        { fbl::count_of(oob_gpio_match), oob_gpio_match },
    };
    device_component_part_t debug_gpio_component[] = {
        { fbl::count_of(root_match), root_match },
        { fbl::count_of(gpio_impl_match), gpio_impl_match },
        { fbl::count_of(debug_gpio_match), debug_gpio_match },
    };
    device_component_t wifi_composite[] = {
        { fbl::count_of(sdio_component), sdio_component },
        { fbl::count_of(oob_gpio_component), oob_gpio_component },
        { fbl::count_of(debug_gpio_component), debug_gpio_component },
    };
    zx_device_prop_t props[] = {
        { BIND_PLATFORM_DEV_VID, 0, PDEV_VID_BROADCOM },
        { BIND_PLATFORM_DEV_PID, 0, PDEV_PID_BCM4356 },
        { BIND_PLATFORM_DEV_DID, 0, PDEV_DID_BCM_WIFI },
    };
    status = DdkAddComposite("vim2-wifi", props, fbl::count_of(props), wifi_composite,
                             fbl::count_of(wifi_composite), 0);
    if (status != ZX_OK) {
        zxlogf(ERROR, "TestBoard::Create: device_add_composite failed: %d\n", status);
    }

    return ZX_OK;
}
} //namespace vim
