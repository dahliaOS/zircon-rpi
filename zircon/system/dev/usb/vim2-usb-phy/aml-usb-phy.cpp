// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-usb-phy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <fbl/algorithm.h>
#include <fbl/unique_ptr.h>
#include <hw/reg.h>

#include <soc/aml-common/aml-usb-phy.h>
#include <soc/aml-s912/s912-hw.h>

//#include "usb-phy-regs.h"

namespace aml_usb_phy {

#define PREI_USB_PHY_2_REG_BASE 0xd0078020
#define PREI_USB_PHY_3_REG_BASE 0xd0078080

typedef struct u2p_aml_regs {
	volatile uint32_t u2p_r0;
	volatile uint32_t u2p_r1;
	volatile uint32_t u2p_r2;
} u2p_aml_regs_t;

typedef union u2p_r0 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned bypass_sel:1;
		unsigned bypass_dm_en:1;
		unsigned bypass_dp_en:1;
		unsigned txbitstuffenh:1;
		unsigned txbitstuffen:1;
		unsigned dmpulldown:1;
		unsigned dppulldown:1;
		unsigned vbusvldextsel:1;
		unsigned vbusvldext:1;
		unsigned adp_prb_en:1;
		unsigned adp_dischrg:1;
		unsigned adp_chrg:1;
		unsigned drvvbus:1;
		unsigned idpullup:1;
		unsigned loopbackenb:1;
		unsigned otgdisable:1;
		unsigned commononn:1;
		unsigned fsel:3;
		unsigned refclksel:2;
		unsigned por:1;
		unsigned vatestenb:2;
		unsigned set_iddq:1;
		unsigned ate_reset:1;
		unsigned fsv_minus:1;
		unsigned fsv_plus:1;
		unsigned bypass_dm_data:1;
		unsigned bypass_dp_data:1;
		unsigned not_used:1;
	} b;
} u2p_r0_t;

typedef struct usb_aml_regs {
	volatile uint32_t usb_r0;
	volatile uint32_t usb_r1;
	volatile uint32_t usb_r2;
	volatile uint32_t usb_r3;
	volatile uint32_t usb_r4;
	volatile uint32_t usb_r5;
	volatile uint32_t usb_r6;
} usb_aml_regs_t;

typedef union usb_r0 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned p30_fsel:6;
		unsigned p30_phy_reset:1;
		unsigned p30_test_powerdown_hsp:1;
		unsigned p30_test_powerdown_ssp:1;
		unsigned p30_acjt_level:5;
		unsigned p30_tx_vboost_lvl:3;
		unsigned p30_lane0_tx2rx_loopbk:1;
		unsigned p30_lane0_ext_pclk_req:1;
		unsigned p30_pcs_rx_los_mask_val:10;
		unsigned u2d_ss_scaledown_mode:2;
		unsigned u2d_act:1;
	} b;
} usb_r0_t;

typedef union usb_r4 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned p21_PORTRESET0:1;
		unsigned p21_SLEEPM0:1;
		unsigned mem_pd:2;
		unsigned reserved4:28;
	} b;
} usb_r4_t;


zx_status_t f_set_usb_phy_config() {
	const int time_dly = 500;

    zx::unowned_resource resource(get_root_resource());

    std::optional<ddk::MmioBuffer> usb_phy;
    auto status = ddk::MmioBuffer::Create(S912_USB_PHY_BASE, S912_USB_PHY_LENGTH, *resource,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE,  &usb_phy);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbInit io_buffer_init_physical failed %d\n", status);
        return status;
    }
    std::optional<ddk::MmioBuffer> preset;
    status = ddk::MmioBuffer::Create(S912_PRESET_BASE, S912_PRESET_LENGTH, *resource,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE,  &preset);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbInit io_buffer_init_physical failed %d\n", status);
        return status;
    }
    std::optional<ddk::MmioBuffer> aobus;
    status = ddk::MmioBuffer::Create(S912_AOBUS_BASE, S912_AOBUS_LENGTH, *resource,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE,  &aobus);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbInit io_buffer_init_physical failed %d\n", status);
        return status;
    }

	volatile u2p_aml_regs_t * u2p_aml_regs = (u2p_aml_regs_t * )((uint8_t*)usb_phy->get() + (PREI_USB_PHY_2_REG_BASE - S912_USB_PHY_BASE));
	volatile usb_aml_regs_t * usb_aml_regs = (usb_aml_regs_t * )((uint8_t*)usb_phy->get() + (PREI_USB_PHY_3_REG_BASE - S912_USB_PHY_BASE));

	u2p_r0_t u2p_r0;
	usb_r0_t usb_r0;
	usb_r4_t usb_r4;

    volatile uint32_t* P_RESET1_REGISTER       = (uint32_t *)((uint8_t*)preset->get() + 0x408);
    volatile uint32_t* P_AO_RTC_ALT_CLK_CNTL0  = (uint32_t *)((uint8_t*)aobus->get() + (0x25 << 2));
    volatile uint32_t* P_AO_RTI_PWR_CNTL_REG0  = (uint32_t *)((uint8_t*)aobus->get() + (0x04 << 2));

printf("f_set_usb_phy_config 1 P_RESET1_REGISTER 0x%p\n", (void*)P_RESET1_REGISTER);
	*P_RESET1_REGISTER = (1<<2);

	*P_AO_RTC_ALT_CLK_CNTL0 |= (1<<31)|(1<<30);
	*P_AO_RTI_PWR_CNTL_REG0 |= (4<<10);

	u2p_r0.d32 = u2p_aml_regs->u2p_r0;
//#if (defined  CONFIG_AML_MESON_GXTVBB)
//	u2p_r0.b.fsel = 5;
//#elif  (defined CONFIG_AML_MESON_GXL)
	u2p_r0.b.fsel = 2;
//#endif
	u2p_r0.b.por = 1;
	u2p_r0.b.dppulldown = 0;
	u2p_r0.b.dmpulldown = 0;
	u2p_aml_regs->u2p_r0 = u2p_r0.d32;

	u2p_r0.d32 = u2p_aml_regs->u2p_r0;
	u2p_r0.b.por = 0;
	u2p_aml_regs->u2p_r0 = u2p_r0.d32;

	usb_r0.d32 = usb_aml_regs->usb_r0;
	usb_r0.b.u2d_act = 1;
	usb_aml_regs->usb_r0 = usb_r0.d32;

	usb_r4.d32 = usb_aml_regs->usb_r4;
	usb_r4.b.p21_SLEEPM0 = 1;
	usb_aml_regs->usb_r4 = usb_r4.d32;

	usleep(time_dly);
	return ZX_OK;
}

zx_status_t AmlUsbPhy::InitPhy() {
    return f_set_usb_phy_config();
}

zx_status_t AmlUsbPhy::Create(void* ctx, zx_device_t* parent) {
    fbl::AllocChecker ac;
    auto dev = fbl::make_unique_checked<AmlUsbPhy>(&ac, parent);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    auto status = dev->Init();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = dev.release();
    return ZX_OK;
}

zx_status_t AmlUsbPhy::AddXhciDevice() {
printf("XXXX %s\n", __func__);
    if (xhci_device_ != nullptr) {
        zxlogf(ERROR, "AmlUsbPhy::AddXhciDevice: device already exists!\n");
        return ZX_ERR_BAD_STATE;
    }

    fbl::AllocChecker ac;
    xhci_device_ = fbl::make_unique_checked<XhciDevice>(&ac, zxdev());
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_GENERIC},
        {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_GENERIC},
        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_USB_XHCI_COMPOSITE},
    };

    return xhci_device_->DdkAdd("xhci", 0, props, countof(props), ZX_PROTOCOL_USB_PHY);
}

zx_status_t AmlUsbPhy::RemoveXhciDevice() {
printf("XXXX %s\n", __func__);
    if (xhci_device_ == nullptr) {
        zxlogf(ERROR, "AmlUsbPhy::RemoveXhciDevice: device does not exist!\n");
        return ZX_ERR_BAD_STATE;
    }

    // devmgr will own the device until it is destroyed.
    __UNUSED auto* dev = xhci_device_.release();
    dev->DdkRemove();

    return ZX_OK;
}

zx_status_t AmlUsbPhy::AddDwc2Device() {
printf("XXXX %s\n", __func__);
    if (dwc2_device_ != nullptr) {
        zxlogf(ERROR, "AmlUsbPhy::AddDwc2Device: device already exists!\n");
        return ZX_ERR_BAD_STATE;
    }

    fbl::AllocChecker ac;
    dwc2_device_ = fbl::make_unique_checked<Dwc2Device>(&ac, zxdev());
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_GENERIC},
        {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_GENERIC},
        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_USB_DWC2},
    };

    return dwc2_device_->DdkAdd("dwc2", 0, props, countof(props), ZX_PROTOCOL_USB_PHY);
}

zx_status_t AmlUsbPhy::RemoveDwc2Device() {
printf("XXXX %s\n", __func__);
    if (dwc2_device_ == nullptr) {
        zxlogf(ERROR, "AmlUsbPhy::RemoveDwc2Device: device does not exist!\n");
        return ZX_ERR_BAD_STATE;
    }

    // devmgr will own the device until it is destroyed.
    __UNUSED auto* dev = dwc2_device_.release();
    dev->DdkRemove();

    return ZX_OK;
}

zx_status_t AmlUsbPhy::Init() {
    if (!pdev_.is_valid()) {
        zxlogf(ERROR, "AmlUsbPhy::Init: could not get platform device protocol\n");
        return ZX_ERR_NOT_SUPPORTED;
    }
/*
    status = pdev_.MapMmio(0, &reset_mmio_);
    if (status != ZX_OK) {
        return status;
    }
    status = pdev_.MapMmio(1, &usbctrl_mmio_);
    if (status != ZX_OK) {
        return status;
    }
    status = pdev_.MapMmio(2, &usbphy20_mmio_);
    if (status != ZX_OK) {
        return status;
    }
    status = pdev_.MapMmio(3, &usbphy21_mmio_);
    if (status != ZX_OK) {
        return status;
    }
    status = pdev_.GetInterrupt(0, &irq_);
    if (status != ZX_OK) {
        return status;
    }
*/
    auto status = InitPhy();
    if (status != ZX_OK) {
        return status;
    }

    status = DdkAdd("aml-usb-phy-v2", DEVICE_ADD_NON_BINDABLE);
    if (status != ZX_OK) {
        return status;
    }

    AddDwc2Device();

    return ZX_OK;
}

void AmlUsbPhy::UsbPhyConnectStatusChanged(bool connected) {
printf("AmlUsbPhy::UsbPhyConnectStatusChanged %d\n", connected);
    dwc2_connected_ = connected;
}

void AmlUsbPhy::DdkUnbind() {
    // Exit IrqThread.
    irq_.destroy();
    thrd_join(irq_thread_, nullptr);

    RemoveXhciDevice();
    RemoveDwc2Device();
    DdkRemove();
}

void AmlUsbPhy::DdkRelease() {
    delete this;
}

static constexpr zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = AmlUsbPhy::Create;
    return ops;
}();

} // namespace aml_usb_phy

ZIRCON_DRIVER_BEGIN(aml_usb_phy, aml_usb_phy::driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_KHADAS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_VIM2),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_VIM_USB_PHY),
ZIRCON_DRIVER_END(aml_usb_phy)
