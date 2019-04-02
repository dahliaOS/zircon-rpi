// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/platform-defs.h>
#include <fbl/alloc_checker.h>
#include <algorithm>

#include "generic-platform-device.h"

namespace platform_device {

zx_status_t PDevDevice::DdkGetProtocol(uint32_t proto_id, void* out) {
    if (proto_id == ZX_PROTOCOL_PDEV) {
        auto* proto = static_cast<pdev_protocol_t*>(out);
        proto->ops = &pdev_protocol_ops_;
        proto->ctx = this;
        return ZX_OK;
    }

    // TODO(voydanoff) remove this after platform bus is fully transitioned to composite devices.
    return device_get_protocol(parent(), proto_id, out);
}

void PDevDevice::DdkUnbind() {
    DdkRemove();
}

void PDevDevice::DdkRelease() {
    delete this;
}

zx_status_t PDevDevice::PDevGetMmio(uint32_t index, pdev_mmio_t* out_mmio) {
    if (index >= mmios_.size()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const Mmio& mmio = mmios_[index];
    const zx_paddr_t vmo_base = ROUNDDOWN(mmio.base, ZX_PAGE_SIZE);
    const size_t vmo_size = ROUNDUP(mmio.base + mmio.length - vmo_base, ZX_PAGE_SIZE);
    zx::vmo vmo;

    zx_status_t status = zx::vmo::create_physical(mmio.resource, vmo_base, vmo_size, &vmo);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s %s: creating vmo failed %d\n", name_.data(), __FUNCTION__, status);
        return status;
    }

    char name[32];
    snprintf(name, sizeof(name), "%s mmio %u", name_.data(), index);
    status = vmo.set_property(ZX_PROP_NAME, name, sizeof(name));
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s %s: setting vmo name failed %d\n", name_.data(), __FUNCTION__, status);
        return status;
    }

    out_mmio->offset = mmio.base - vmo_base;
    out_mmio->vmo = vmo.release();
    out_mmio->size = mmio.length;
    return ZX_OK;
}

zx_status_t PDevDevice::PDevGetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out_irq) {
    if (index >= irqs_.size()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    Irq* irq = &irqs_[index];
    if (flags == 0) {
        flags = irq->mode;
    }
    zx_status_t status = zx::interrupt::create(irq->resource, irq->irq, flags, out_irq);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s %s: creating interrupt failed: %d\n", name_.data(), __FUNCTION__, status);
        return status;
    }

    return ZX_OK;
}

zx_status_t PDevDevice::PDevGetBti(uint32_t index, zx::bti* out_bti) {
    return pdev_.GetBti(index, out_bti);
}

zx_status_t PDevDevice::PDevGetSmc(uint32_t index, zx::resource* out_smc) {
    return pdev_.GetSmc(index, out_smc);
}

zx_status_t PDevDevice::PDevGetDeviceInfo(pdev_device_info_t* out_info) {
    return pdev_.GetDeviceInfo(out_info);
}

zx_status_t PDevDevice::PDevGetBoardInfo(pdev_board_info_t* out_info) {
    return pdev_.GetBoardInfo(out_info);
}

zx_status_t PDevDevice::PDevDeviceAdd(uint32_t index, const device_add_args_t* args,
                                      zx_device_t** out_device) {
    return pdev_.DeviceAdd(index, args, out_device);
}

zx_status_t PDevDevice::PDevGetProtocol(uint32_t proto_id, uint32_t index, void* buffer,
                                        size_t buffer_size, size_t* out_actual) {
    return pdev_.GetProtocol(proto_id, index, buffer, buffer_size, out_actual);
}

zx_status_t PDevDevice::Create(void* ctx, zx_device_t* parent) {
    pdev_impl_protocol_t pdev;
    auto status = device_get_protocol(parent, ZX_PROTOCOL_PDEV_IMPL, &pdev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: could not get ZX_PROTOCOL_PDEV_IMPL\n", __func__);
        return status;
    }

    pdev_device_info_t info;
    status = pdev_impl_get_device_info(&pdev, &info);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_impl_get_device_info failed\n", __func__);
        return status;
    }

    fbl::Vector<Mmio> mmios;
    fbl::Vector<Irq> irqs;
    fbl::AllocChecker ac;

    for (uint32_t i = 0; i < info.mmio_count; i++) {
        Mmio mmio;

        status = pdev_impl_get_mmio(&pdev, i, &mmio.base, &mmio.length,
                                    mmio.resource.reset_and_get_address());
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s: pdev_impl_get_mmio failed\n", __func__);
            return status;
        }

        mmios.push_back(std::move(mmio), &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }

        zxlogf(SPEW, "%s: received MMIO %u (base %#lx length %#lx handle %#x)\n", info.name, i,
               mmio.base, mmio.length, mmio.resource.get());
    }

    for (uint32_t i = 0; i < info.irq_count; i++) {
        Irq irq;

        status = pdev_impl_get_interrupt(&pdev, i, &irq.irq, &irq.mode,
                                         irq.resource.reset_and_get_address());
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s: pdev_impl_get_interrupt failed\n", __func__);
            return status;
        }

        irqs.push_back(std::move(irq), &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }

        zxlogf(SPEW, "%s: received IRQ %u (irq %#x handle %#x)\n", info.name, i, irq.irq,
               irq.resource.get());
    }

    std::unique_ptr<PDevDevice> dev(new (&ac) PDevDevice(parent, &pdev, info.name, std::move(mmios),
                                                         std::move(irqs)));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, info.vid},
        {BIND_PLATFORM_DEV_PID, 0, info.pid},
        {BIND_PLATFORM_DEV_DID, 0, info.did},
    };

    char name[ZX_DEVICE_NAME_MAX];
    snprintf(name, sizeof(name), "%u:%u:%u", info.vid, info.pid, info.did);

    status = dev->DdkAdd(name, 0, props, countof(props));
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DdkAdd failed\n", __func__);
        return status;
    }

    // dev is now owned by devmgr.
    __UNUSED auto ptr = dev.release();

    return ZX_OK;
}

static zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops;
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = PDevDevice::Create;
    return ops;
}();

} // namespace platform_device

ZIRCON_DRIVER_BEGIN(platform_device, platform_device::driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_PDEV_IMPL),
ZIRCON_DRIVER_END(platform_device)
