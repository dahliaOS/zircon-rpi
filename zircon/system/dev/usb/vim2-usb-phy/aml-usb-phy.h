// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_USB_VIM2_USB_PHY_AML_USB_PHY_H_
#define ZIRCON_SYSTEM_DEV_USB_VIM2_USB_PHY_AML_USB_PHY_H_

#include <threads.h>

#include <ddktl/device.h>
#include <lib/device-protocol/pdev.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/interrupt.h>

#include "dwc2-device.h"
#include "xhci-device.h"

namespace aml_usb_phy {

class AmlUsbPhy;
using AmlUsbPhyType = ddk::Device<AmlUsbPhy, ddk::Unbindable>;

// This is the main class for the platform bus driver.
class AmlUsbPhy : public AmlUsbPhyType {
 public:
  explicit AmlUsbPhy(zx_device_t* parent) : AmlUsbPhyType(parent), pdev_(parent) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // Device protocol implementation.
  void DdkUnbind();
  void DdkRelease();

 private:
  enum class UsbMode {
    UNKNOWN,
    HOST,
    PERIPHERAL,
  };

  DISALLOW_COPY_ASSIGN_AND_MOVE(AmlUsbPhy);

  zx_status_t InitPhy();
  void SetMode(bool host);

  zx_status_t AddXhciDevice();
  zx_status_t RemoveXhciDevice();
  zx_status_t AddDwc2Device();
  zx_status_t RemoveDwc2Device();

  zx_status_t Init();

  ddk::PDev pdev_;
  std::optional<ddk::MmioBuffer> usbphy_mmio_;

  // Device node for binding XHCI driver.
  std::unique_ptr<XhciDevice> xhci_device_;
  std::unique_ptr<Dwc2Device> dwc2_device_;

  UsbMode mode_ = UsbMode::UNKNOWN;
  bool dwc2_connected_ = false;
};

}  // namespace aml_usb_phy

#endif  // ZIRCON_SYSTEM_DEV_USB_VIM2_USB_PHY_AML_USB_PHY_H_
