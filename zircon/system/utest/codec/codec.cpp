// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/platform-defs.h>
#include <ddktl/protocol/codec.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/vmo.h>
#include <zxtest/zxtest.h>
#include <zircon/syscalls.h>


using driver_integration_test::IsolatedDevmgr;

namespace {

// Integration test for the driver defined in zircon/system/dev/virtual_camera.
// This test code loads the driver into an isolated devmgr and tests behavior.
class CodecTest : public zxtest::Test {
    void SetUp() override;

protected:
    IsolatedDevmgr devmgr_;
    fbl::unique_fd fd_;
    zx_handle_t device_handle_;
    zx::vmo vmos_0_[64];
};

const board_test::DeviceEntry kDeviceEntry = []() {
    board_test::DeviceEntry entry = {};
    entry.vid = PDEV_VID_TI;
    entry.did = PDEV_DID_TI_TAS5805;
    return entry;
}();

void CodecTest::SetUp() {
    IsolatedDevmgr::Args args;
    args.driver_search_paths.push_back("/boot/driver");
    args.driver_search_paths.push_back("/boot/driver/test");
    args.device_list.push_back(kDeviceEntry);
    zx_status_t status = IsolatedDevmgr::Create(&args, &devmgr_);
    ASSERT_EQ(status, ZX_OK);

    status = devmgr_integration_test::RecursiveWaitForFile(
        devmgr_.devfs_root(), "sys/platform/11:05:b/virtual_camera",
        zx::time::infinite(), &fd_);
    ASSERT_EQ(ZX_OK, status);

    status = fdio_get_service_handle(fd_.get(), &device_handle_);
    ASSERT_EQ(ZX_OK, status);
}

TEST_F(CodecTest, Reset) {
    ddk::CodecProtocolClient proto_client_;
    gain_state_t state = {};
    proto_client_.SetGainState(&state, [](void* ctx) {}, nullptr);

}

} // namespace
