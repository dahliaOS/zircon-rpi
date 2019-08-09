// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <fbl/alloc_checker.h>

#include "test-metadata.h"

class TestPowerDriverChild;
using DeviceType = ddk::Device<TestPowerDriverChild, ddk::Unbindable>;
class TestPowerDriverChild : public DeviceType {
 public:
  TestPowerDriverChild(zx_device_t* parent) : DeviceType(parent) {}
  static zx_status_t Create(void* ctx, zx_device_t* device);
  zx_status_t Bind();
  void DdkUnbind() {
    DdkRemove();
  }
  void DdkRelease() { delete this; }
  struct power_test_metadata test_metadata_ = {};
};

zx_status_t TestPowerDriverChild::Bind() {
  size_t actual;
  auto status =
      DdkGetMetadata(DEVICE_METADATA_PRIVATE, &test_metadata_, sizeof(test_metadata_), &actual);
  if (status != ZX_OK || actual != sizeof(test_metadata_)) {
    zxlogf(ERROR, "Getting metadata for power test child not succesful\n");
    return ZX_ERR_INTERNAL;
  }
  return DdkAdd("power-test-child");
}

zx_status_t TestPowerDriverChild::Create(void* ctx, zx_device_t* device) {
  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<TestPowerDriverChild>(&ac, device);
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

static zx_driver_ops_t test_power_child_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops;
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = TestPowerDriverChild::Create;
  return ops;
}();

// clang-format off
ZIRCON_DRIVER_BEGIN(TestPowerChild, test_power_child_driver_ops, "zircon", "0.1", 1)
  BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_TEST_CHILD),
ZIRCON_DRIVER_END(TestPowerChild)
// clang-format on
