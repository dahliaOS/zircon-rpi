// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/gpu/magma/c/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>

#include <shared_mutex>
#include <thread>

#include <gtest/gtest.h>

#include "helper/test_device_helper.h"
#include "magma.h"
#include "magma_util/macros.h"
#include "magma_vendor_queries.h"

// The test build of the MSD runs a bunch of unit tests automatically when it loads. We need to
// unload the normal MSD to replace it with the test MSD so we can run those tests and query the
// test results.
TEST(UnitTests, UnitTests) {
  auto test_base = std::make_unique<magma::TestDeviceBase>(MAGMA_VENDOR_ID_MALI);
  zx::channel parent_device = test_base->GetParentDevice();

  test_base->ShutdownDevice();
  test_base.reset();

  const char* kTestDriverPath = "/system/driver/libmsd_arm_test.so";
  // The test driver will run unit tests on startup.
  magma::TestDeviceBase::BindDriver(parent_device, kTestDriverPath);

  test_base = std::make_unique<magma::TestDeviceBase>(MAGMA_VENDOR_ID_MALI);
  zx_status_t status, status2 = ZX_OK;
  status = fuchsia_gpu_magma_DeviceGetUnitTestStatus(test_base->channel()->get(), &status2);
  EXPECT_EQ(ZX_OK, status) << "Device connection lost, check syslog for any errors.";
  EXPECT_EQ(ZX_OK, status2) << "Tests reported errors, check syslog.";

  test_base->ShutdownDevice();
  test_base.reset();

  // Reload the production driver so later tests shouldn't be affected.
  const char* kDriverPath = "/system/driver/libmsd_arm.so";
  magma::TestDeviceBase::BindDriver(parent_device, kDriverPath);
}
