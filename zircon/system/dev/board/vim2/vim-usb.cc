// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/resource.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/usb-peripheral-config.h>
#include <hw/reg.h>
#include <soc/aml-s912/s912-hw.h>
#include <usb/dwc2/metadata.h>
#include <zircon/device/usb-peripheral.h>
#include <zircon/hw/usb.h>
#include <zircon/hw/usb/cdc.h>

#include "vim.h"

namespace vim {

static const pbus_mmio_t usb_phy_mmios[] = {
    {
        .base = S912_USB_PHY_BASE,
        .length = S912_USB_PHY_LENGTH,
    },
};

static const pbus_mmio_t xhci_mmios[] = {
    {
        .base = S912_USB0_BASE,
        .length = S912_USB0_LENGTH,
    },
};

static const pbus_irq_t xhci_irqs[] = {
    {
        .irq = S912_USBH_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const pbus_bti_t xhci_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_USB,
    },
};

static const pbus_mmio_t dwc2_mmios[] = {
    {
        .base = S912_USB1_BASE,
        .length = S912_USB1_LENGTH,
    },
};

static const pbus_irq_t dwc2_irqs[] = {
    {
        .irq = S912_USBD_IRQ,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
};

static const pbus_bti_t usb_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_USB,
    },
};

constexpr char kManufacturer[] = "Zircon";
constexpr char kProduct[] = "CDC-Ethernet";
constexpr char kSerial[] = "0123456789ABCDEF";

// Metadata for DWC2 driver.
constexpr dwc2_metadata_t dwc2_metadata = {
    .dma_burst_len = DWC2_DMA_BURST_INCR8,
    .usb_turnaround_time = 5,
    .rx_fifo_size = 256,
    .nptx_fifo_size = 256,
};

using FunctionDescriptor = fuchsia_hardware_usb_peripheral_FunctionDescriptor;

static pbus_metadata_t usb_metadata[] = {
    {
        .type = DEVICE_METADATA_USB_CONFIG,
        .data_buffer = nullptr,
        .data_size = 0,
    },
    {
        .type = DEVICE_METADATA_PRIVATE,
        .data_buffer = &dwc2_metadata,
        .data_size = sizeof(dwc2_metadata),
    },
};

static const zx_bind_inst_t root_match[] = {
    BI_MATCH(),
};

static const zx_bind_inst_t xhci_phy_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_USB_PHY),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_USB_XHCI_COMPOSITE),
};
static const device_component_part_t xhci_phy_component[] = {
    { countof(root_match), root_match },
    { countof(xhci_phy_match), xhci_phy_match },
};
static const device_component_t xhci_components[] = {
    { countof(xhci_phy_component), xhci_phy_component },
};

static const zx_bind_inst_t dwc2_phy_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_USB_PHY),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_USB_DWC2),
};
static const device_component_part_t dwc2_phy_component[] = {
    { countof(root_match), root_match },
    { countof(dwc2_phy_match), dwc2_phy_match },
};
static const device_component_t dwc2_components[] = {
    { countof(dwc2_phy_component), dwc2_phy_component },
};

static const pbus_dev_t usb_phy_dev = [](){
    pbus_dev_t dev;
    dev.name = "aml-usb-phy-v2";
    dev.vid = PDEV_VID_KHADAS;
    dev.pid = PDEV_PID_VIM2;
    dev.did = PDEV_DID_VIM_USB_PHY;
    dev.mmio_list = usb_phy_mmios;
    dev.mmio_count = countof(usb_phy_mmios);
    dev.bti_list = usb_btis;
    dev.bti_count = countof(usb_btis);
    return dev;
}();

static const pbus_dev_t xhci_dev = [](){
    pbus_dev_t dev;
    dev.name = "xhci";
    dev.vid = PDEV_VID_GENERIC;
    dev.pid = PDEV_PID_GENERIC;
    dev.did = PDEV_DID_USB_XHCI;
    dev.mmio_list = xhci_mmios;
    dev.mmio_count = countof(xhci_mmios);
    dev.irq_list = xhci_irqs;
    dev.irq_count = countof(xhci_irqs);
    dev.bti_list = xhci_btis;
    dev.bti_count = countof(xhci_btis);
    return dev;
}();

static const pbus_dev_t dwc2_dev = [](){
    pbus_dev_t dev;
    dev.name = "dwc2";
    dev.vid = PDEV_VID_GENERIC;
    dev.pid = PDEV_PID_GENERIC;
    dev.did = PDEV_DID_USB_DWC2;
    dev.mmio_list = dwc2_mmios;
    dev.mmio_count = countof(dwc2_mmios);
    dev.irq_list = dwc2_irqs;
    dev.irq_count = countof(dwc2_irqs);
    dev.bti_list = usb_btis;
    dev.bti_count = countof(usb_btis);
    dev.metadata_list = usb_metadata;
    dev.metadata_count = countof(usb_metadata);
    return dev;
}();

zx_status_t Vim::UsbInit() {
    auto status = pbus_.DeviceAdd(&usb_phy_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DeviceAdd failed %d\n", __func__, status);
        return status;
    }

    constexpr size_t alignment = alignof(UsbConfig) > __STDCPP_DEFAULT_NEW_ALIGNMENT__
                                     ? alignof(UsbConfig)
                                     : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    constexpr size_t config_size = sizeof(UsbConfig) + 2 * sizeof(FunctionDescriptor);
    UsbConfig* config = reinterpret_cast<UsbConfig*>(
        aligned_alloc(alignment, ROUNDUP(config_size, alignment)));
    if (!config) {
        return ZX_ERR_NO_MEMORY;
    }
    config->vid = GOOGLE_USB_VID;
    config->pid = GOOGLE_USB_CDC_AND_FUNCTION_TEST_PID;
    strcpy(config->manufacturer, kManufacturer);
    strcpy(config->serial, kSerial);
    strcpy(config->product, kProduct);
    config->functions[0].interface_class = USB_CLASS_COMM;
    config->functions[0].interface_subclass = USB_CDC_SUBCLASS_ETHERNET;
    config->functions[0].interface_protocol = 0;
    config->functions[1].interface_class = USB_CLASS_VENDOR;
    config->functions[1].interface_subclass = 0;
    config->functions[1].interface_protocol = 0;
    usb_metadata[0].data_size = config_size;
    usb_metadata[0].data_buffer = config;

    status = pbus_.CompositeDeviceAdd(&dwc2_dev, dwc2_components, countof(dwc2_components), 1);
    free(config);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DeviceAdd failed: %d\n", __func__, status);
        return status;
    }

    status = pbus_.CompositeDeviceAdd(&xhci_dev, xhci_components, countof(xhci_components), 1);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DeviceAdd failed: %d\n", __func__, status);
        return status;
    }

    return ZX_OK;
}
} //namespace vim
