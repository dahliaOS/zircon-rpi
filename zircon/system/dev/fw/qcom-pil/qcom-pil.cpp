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

zx_status_t PilDevice::Bind() {
    auto status = pdev_.GetSmc(0, &smc_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s GetSmc failed %d\n", __func__, status);
        return status;
    }
    status = pdev_.GetBti(0, &bti_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s GetSmc failed %d\n", __func__, status);
        return status;
    }

    size_t size = 16 * 1024 * 1024;
    status = zx_vmo_create_contiguous(bti_.get(), size, 0,
                                      buffer_.reset_and_get_address());
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
    status =
        load_firmware(parent(), "adsp.mbn", buffer_.reset_and_get_address(), &size);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: Failed to load firmware\n", __FILE__);
        return status;
    }
    zxlogf(INFO, "%s FW size %lu\n", __func__, size);
    uint8_t b[4096];
    buffer_.read(b, 0, 4096);
    uint8_t* p = (uint8_t*)b;
    for (int i = 0; i < 64; i += 4) {
        zxlogf(INFO, "%02X%02X%02X%02X\n", *(p+i+0), *(p+i+1), *(p+i+2), *(p+i+3));
    }
    zx_smc_parameters_t params = CreatePilSmcParams(
        Cmd::InitImage, CreateScmArgs(2, 0, 2),
        PasId::Q6, static_cast<zx_paddr_t>(pinned_buffer_.region(0).phys_addr));
    zx_smc_result_t result = {};
    zx_smc_call(smc_.get(), &params, &result);
    zxlogf(INFO, "%s ADSP init reply %ld %ld\n", __func__, result.arg0, result.arg1);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));

// Used to test communication with QSEE and its replies for different image ids.
#define TEST_SMC
#ifdef TEST_SMC
    for (int i = 1; i < 16; ++i) {
        zx_smc_parameters_t params = CreatePilSmcParams(Cmd::QuerySupport,
                                                        1,
                                                        static_cast<PasId>(i));
        zx_smc_result_t result = {};
        zx_smc_call(smc_.get(), &params, &result);
        if (result.arg0 == 0 && result.arg1 == 1) {
            zxlogf(INFO, "%s pas_id %d supported\n", __func__, i);
        }
    }
    for (int i = 1; i < 16; ++i) {
        zx_smc_parameters_t params = CreatePilSmcParams(
            Cmd::InitImage, CreateScmArgs(2, 0, 2), static_cast<PasId>(i),
            static_cast<zx_paddr_t>(pinned_buffer_.region(0).phys_addr));
        zx_smc_result_t result = {};
        zx_smc_call(smc_.get(), &params, &result);
        zxlogf(INFO, "%s pas_id %d init reply %ld %ld\n", __func__, i, result.arg0,
               result.arg1);
        zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
    }
    for (int i = 1; i < 16; ++i) {
        zx_smc_parameters_t params = CreatePilSmcParams(
            Cmd::AuthAndReset, CreateScmArgs(1, 0), static_cast<PasId>(i),
            static_cast<zx_paddr_t>(pinned_buffer_.region(0).phys_addr));
        zx_smc_result_t result = {};
        zx_smc_call(smc_.get(), &params, &result);
        zxlogf(INFO, "%s pas_id %d auth and reset reply %ld %ld\n", __func__, i, result.arg0,
               result.arg1);
        zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
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
    fbl::AllocChecker ac;
    auto dev = fbl::make_unique_checked<PilDevice>(&ac, parent);
    if (!ac.check()) {
        zxlogf(ERROR, "%s PilDevice creation ZX_ERR_NO_MEMORY\n", __func__);
        return ZX_ERR_NO_MEMORY;
    }
    auto status = dev->Bind();
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
