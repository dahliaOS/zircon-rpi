// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/platform/device.h>
#include <ddktl/protocol/platform/deviceimpl.h>
#include <fbl/string.h>
#include <fbl/vector.h>

namespace platform_device {

struct Mmio {
    zx_paddr_t base;
    size_t length;
    zx::resource resource;
};

struct Irq {
    uint32_t irq;
    // ZX_INTERRUPT_MODE_* flags
    uint32_t mode;
    zx::resource resource;
};

class PDevDevice;
using PDevDeviceType = ddk::Device<PDevDevice, ddk::GetProtocolable, ddk::Unbindable>;

class PDevDevice : public PDevDeviceType,
                     public ddk::PDevProtocol<PDevDevice, ddk::base_protocol> {
public:
    PDevDevice(zx_device_t* parent, pdev_impl_protocol_t* pdev, const char* name,
               fbl::Vector<Mmio>&& mmios, fbl::Vector<Irq>&& irqs)
        : PDevDeviceType(parent), pdev_(pdev), name_(name), mmios_(std::move(mmios)),
          irqs_(std::move(irqs)) {}

    static zx_status_t Create(void* ctx, zx_device_t* parent);

    zx_status_t DdkGetProtocol(uint32_t proto_id, void* out);
    void DdkUnbind();
    void DdkRelease();

    zx_status_t PDevGetMmio(uint32_t index, pdev_mmio_t* out_mmio);
    zx_status_t PDevGetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out_irq);
    zx_status_t PDevGetBti(uint32_t index, zx::bti* out_bti);
    zx_status_t PDevGetSmc(uint32_t index, zx::resource* out_smc);
    zx_status_t PDevGetDeviceInfo(pdev_device_info_t* out_info);
    zx_status_t PDevGetBoardInfo(pdev_board_info_t* out_info);
    zx_status_t PDevDeviceAdd(uint32_t index, const device_add_args_t* args,
                              zx_device_t** out_device);
    zx_status_t PDevGetProtocol(uint32_t proto_id, uint32_t index, void* out_out_protocol_buffer,
                                size_t out_protocol_size, size_t* out_out_protocol_actual);

private:
    const ddk::PDevImplProtocolClient pdev_;
    fbl::String name_;
    fbl::Vector<Mmio> mmios_;
    fbl::Vector<Irq> irqs_;
};

} // namespace platform_device
