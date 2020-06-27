// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/device/llcpp/fidl.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/devmgr-launcher/launch.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/zx/channel.h>
#include <sys/types.h>

#include <cstdint>
#include <utility>

#include <fvm/test/device-ref.h>
#include <ramdevice-client/ramdisk.h>
#include <zxtest/zxtest.h>

namespace {

constexpr uint64_t kBlockSize = 512;
constexpr uint64_t kBlockCount = 32;  // TODO real numbers

const char* kDriverLib = "block-verity.so";

TEST(BlockVerityTest, Bind) {
  zx_status_t rc;
  // Set up isolated devmgr.
  driver_integration_test::IsolatedDevmgr::Args args;
  args.driver_search_paths.push_back("/pkg/drivers");
  args.driver_search_paths.push_back("/boot/driver");
  args.driver_search_paths.push_back("/system/driver");
  std::unique_ptr<driver_integration_test::IsolatedDevmgr> devmgr_ = nullptr;
  ASSERT_OK(driver_integration_test::IsolatedDevmgr::Create(&args, devmgr_.get()));
  // Create ramdisk.
  std::unique_ptr<fvm::RamdiskRef> ramdisk =
      fvm::RamdiskRef::Create(devmgr_->devfs_root(), kBlockSize, kBlockCount);
  ASSERT_TRUE(ramdisk);
  // Bind the driver to the ramdisk.
  auto resp = ::llcpp::fuchsia::device::Controller::Call::Bind(
      zx::unowned_channel(ramdisk->channel()), ::fidl::unowned_str(kDriverLib, strlen(kDriverLib)));
  rc = resp.status();
  if (rc == ZX_OK) {
    if (resp->result.is_err()) {
      rc = resp->result.err();
    }
  }
  ASSERT_TRUE(rc == ZX_OK);

  // Teardown.
  devmgr_.reset();
}

}  // namespace
