// Copyright 2019 The Fuchsia Authors. All rights reserved.
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

namespace aml_usb_phy {

zx_status_t AmlUsbPhy::InitPhy() {
  auto* mmio = &*usbphy_mmio_;

  for (int i = 0; i < 3; i++) {
    if (i == 1) {
      // Configure port 1 for peripheral role
      U2P_R0::Get(i)
        .ReadFrom(mmio)
        .set_fsel(2)
        .set_por(1)
        .set_dmpulldown(0)
        .set_dppulldown(0)
        .set_idpullup(1)
        .WriteTo(mmio);
    } else {
      U2P_R0::Get(i)
        .ReadFrom(mmio)
        .set_por(1)
        .set_dmpulldown(1)
        .set_dppulldown(1)
        .set_idpullup(i == 1)
        .WriteTo(mmio);
    }

    zx_nanosleep(zx_deadline_after(ZX_USEC(500)));

    U2P_R0::Get(i).ReadFrom(mmio).set_por(0).WriteTo(mmio);
  }

  USB_R0::Get()
    .ReadFrom(mmio)
    .set_u2d_act(1)
    .WriteTo(mmio);

  USB_R4::Get()
    .ReadFrom(mmio)
    .set_p21_sleepm0(1)
    .WriteTo(mmio);

  USB_R1::Get()
    .ReadFrom(mmio)
    .set_u3h_fladj_30mhz_reg(0x20)
    .WriteTo(mmio);

  USB_R5::Get()
    .ReadFrom(mmio)
    .set_iddig_en0(1)
    .set_iddig_en1(1)
    .set_iddig_th(255)
    .WriteTo(mmio);

  return ZX_OK;
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

  auto status = pdev_.MapMmio(0, &usbphy_mmio_);
  if (status != ZX_OK) {
    return status;
  }

  status = InitPhy();
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

void AmlUsbPhy::DdkUnbind() {
  RemoveXhciDevice();
  RemoveDwc2Device();
  DdkRemove();
}

void AmlUsbPhy::DdkRelease() { delete this; }

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = AmlUsbPhy::Create;
  return ops;
}();

}  // namespace aml_usb_phy

// clang-format off
ZIRCON_DRIVER_BEGIN(aml_usb_phy, aml_usb_phy::driver_ops, "zircon", "0.1", 4)
BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
  BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_KHADAS),
  BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_VIM2),
  BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_VIM_USB_PHY),
ZIRCON_DRIVER_END(aml_usb_phy)
