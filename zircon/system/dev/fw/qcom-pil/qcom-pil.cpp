// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <fbl/alloc_checker.h>
#include <fbl/unique_ptr.h>
#include <zircon/syscalls/smc.h>

#include "qcom-pil.h"

namespace qcom_pil {

zx_status_t PilDevice::SmcCall(zx_smc_parameters_t* params, zx_smc_result_t* result) {
    zxlogf(INFO, "SMC params %08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
           static_cast<uint32_t>(params->func_id), static_cast<uint32_t>(params->arg1),
           static_cast<uint32_t>(params->arg2), static_cast<uint32_t>(params->arg3),
           static_cast<uint32_t>(params->arg4), static_cast<uint32_t>(params->arg5));
    auto r = zx_smc_call(smc_.get(), params, result);
    zxlogf(INFO, "SMC results 0x%08X 0x%08X\n", static_cast<uint32_t>(result->arg0),
           static_cast<uint32_t>(result->arg1));
    if (result->arg0 == 1) {
        zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
        r = zx_smc_call(smc_.get(), params, result);
        zxlogf(INFO, "SMC retry results %d 0x%08X\n", static_cast<uint32_t>(result->arg0),
               static_cast<uint32_t>(result->arg1));
    }
    return r;
}

zx_status_t PilDevice::Bind() {
    auto status = pdev_.GetSmc(0, &smc_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s GetSmc failed %d\n", __func__, status);
        return status;
    }
    status = pdev_.GetBti(0, &bti_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s GetBti failed %d\n", __func__, status);
        return status;
    }

    constexpr uint32_t apcs_clock_branch_enable_vote_offset = 0x45004;
    mmio_.SetBit<uint32_t>(2, apcs_clock_branch_enable_vote_offset);
    mmio_.SetBit<uint32_t>(1, apcs_clock_branch_enable_vote_offset);
    mmio_.SetBit<uint32_t>(0, apcs_clock_branch_enable_vote_offset);

    zx_paddr_t pa = 0x88400000;
    size_t size = 17 * 1024 * 1024;
    status = zx_vmo_create_physical(get_root_resource(), pa, size, buffer_.reset_and_get_address());
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: Failed to create physical %d\n", __FILE__, status);
        return status;
    }
    status = pinned_buffer_.Pin(buffer_, bti_, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s failed to pin ring buffer vmo - %d\n", __func__, status);
        return status;
    }
    if (pinned_buffer_.region_count() != 1) {
        zxlogf(ERROR, "%s buffer is not contiguous", __func__);
        return ZX_ERR_NO_MEMORY;
    }
    zxlogf(INFO, "%s phys addr 0x%016lX  size %lu\n", __func__, pinned_buffer_.region(0).phys_addr,
        pinned_buffer_.region(0).size);
    zx_vaddr_t address = 0;
    zx_vmar_map(zx_vmar_root_self(),  ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                0, buffer_.get(), 0, size, &address);
    if (status != ZX_OK) {
        zxlogf(ERROR, "vmar failed %d\n", status);
    }

    size_t actual = 0;
    zx::vmo temp;
    status = load_firmware(parent(), "adsp.mdt", temp.reset_and_get_address(), &actual);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: Failed to load firmware\n", __FILE__);
        return status;
    }
    zxlogf(INFO, "buffer %lu FW size %lu\n", size, actual);

    memset((void*)address, 0xFF, size);
    status = temp.read((void*)address, 0, actual);
    if (status != ZX_OK) {
        zxlogf(ERROR, "temp read failed %d\n", status);
    }
    zx_nanosleep(zx_deadline_after(ZX_MSEC(100)));
    zx_smc_parameters_t params = {};
    zx_smc_result_t result = {};

    params = CreateSmcParams(CreateFunctionId(kYieldingCall, kSmc32CallConv, kSipService,
                                              static_cast<uint8_t>(TzService::Info),
                                              static_cast<uint8_t>(3)),
                             CreateScmArgs(1, 0), 10);
    result = {};
    SmcCall(&params, &result);

    params = CreatePilSmcParams(PilCmd::InitImage, CreateScmArgs(2, 0, 2),
                                PasId::Q6, static_cast<zx_paddr_t>(pa));
    SmcCall(&params, &result);


// Used to test communication with QSEE and its replies for different image ids.
#define TEST_SMC
#ifdef TEST_SMC
    for (int i = 1; i < 16; ++i) {
        zx_smc_parameters_t params = CreatePilSmcParams(PilCmd::QuerySupport,
                                                        1,
                                                        static_cast<PasId>(i));
        zx_smc_result_t result = {};
        zx_smc_call(smc_.get(), &params, &result);
        if (result.arg0 == 0 && result.arg1 == 1) {
            zxlogf(INFO, "%s pas_id %d supported\n", __func__, i);
        }
    }
#endif
    status = DdkAdd("qcom-pil");
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s DdkAdd failed %d\n", __func__, status);
        ShutDown();
        return status;
    }
    return ZX_OK;
}
zx_status_t PilDevice::Init() {
    return ZX_OK;
}

void PilDevice::ShutDown() {
}

void PilDevice::DdkUnbind() {
    ShutDown();
    DdkRemove();
}

void PilDevice::DdkRelease() {
    delete this;
}

zx_status_t PilDevice::Create(zx_device_t* parent) {
    pdev_protocol_t pdev;
    zx_status_t status = device_get_protocol(parent, ZX_PROTOCOL_PDEV, &pdev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s ZX_PROTOCOL_PDEV not available %d\n", __func__, status);
        return status;
    }

    mmio_buffer_t mmio;
    status = pdev_map_mmio_buffer(&pdev, 0, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s gpio pdev_map_mmio_buffer failed %d\n", __func__, status);
        return status;
    }

    fbl::AllocChecker ac;
    auto dev = fbl::make_unique_checked<PilDevice>(&ac, parent, mmio);
    if (!ac.check()) {
        zxlogf(ERROR, "%s PilDevice creation ZX_ERR_NO_MEMORY\n", __func__);
        return ZX_ERR_NO_MEMORY;
    }
    status = dev->Bind();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the memory for dev
    auto ptr = dev.release();
    return ptr->Init();
}

zx_status_t qcom_pil_bind(void* ctx, zx_device_t* parent) {
    return qcom_pil::PilDevice::Create(parent);
}

} // namespace qcom_pil
