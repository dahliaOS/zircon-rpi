// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <fbl/alloc_checker.h>

class TestCompatibilityHookDriverChild;
using DeviceType = ddk::Device<TestCompatibilityHookDriverChild, ddk::Unbindable>;
class TestCompatibilityHookDriverChild : public DeviceType {
public:
    TestCompatibilityHookDriverChild(zx_device_t* parent)
        : DeviceType(parent) {}
    zx_status_t Bind();
    void DdkUnbind() {
        printf("CHILD DEVICE UNBIND\n");
        DdkRemove();
    }
    void DdkRelease() {
        printf("CHILD DEVICE RELEASE\n");
        delete this;
    }
};

zx_status_t TestCompatibilityHookDriverChild::Bind() {
    printf("CHILD DEVICE BIND\n");
    return DdkAdd("compatibility-test-child");
}

zx_status_t test_compatibility_hook_child_bind(void* ctx, zx_device_t* device) {
    fbl::AllocChecker ac;
    auto dev = fbl::make_unique_checked<TestCompatibilityHookDriverChild>(&ac, device);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    auto status = dev->Bind();
    if (status == ZX_OK) {
        // devmgr is now in charge of the memory for dev
        __UNUSED auto ptr = dev.release();
    }
    return status;
}

static zx_driver_ops_t test_compatibility_hook_child_driver_ops = []() -> zx_driver_ops_t {
    zx_driver_ops_t ops;
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = test_compatibility_hook_child_bind;
    return ops;
}();

// clang-format off
ZIRCON_DRIVER_BEGIN(TestCompatibilityHookChild, test_compatibility_hook_child_driver_ops, "zircon", "0.1", 4)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_TEST_CHILD),
ZIRCON_DRIVER_END(TestCompatibilityHookChild)
// clang-format on
