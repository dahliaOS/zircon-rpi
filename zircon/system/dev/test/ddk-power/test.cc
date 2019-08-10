#include <ddk/platform-defs.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/namespace.h>
#include <lib/fdio/spawn.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zxtest/zxtest.h>

#include "test-metadata.h"

using driver_integration_test::IsolatedDevmgr;

TEST(DeviceControllerIntegrationTest, InvalidDeviceCaps) {
  IsolatedDevmgr devmgr;
  IsolatedDevmgr::Args args;
  args.load_drivers.push_back("/boot/driver/ddk-power-test.so");
  args.load_drivers.push_back("/boot/driver/ddk-power-test-child.so");

  board_test::DeviceEntry dev = {};
  struct power_test_metadata test_metadata = {
      .num_config = 1,
  };
  dev.metadata = reinterpret_cast<const uint8_t *>(&test_metadata);
  dev.metadata_size = sizeof(test_metadata);
  dev.vid = PDEV_VID_TEST;
  dev.pid = PDEV_PID_POWER_TEST;
  dev.did = 0;
  args.device_list.push_back(dev);

  zx_status_t status = IsolatedDevmgr::Create(&args, &devmgr);
  ASSERT_OK(status);
  fbl::unique_fd parent_fd, child_fd;
  devmgr_integration_test::RecursiveWaitForFile(
      devmgr.devfs_root(), "sys/platform/11:0b:0/power-test", &parent_fd);
  ASSERT_GT(parent_fd.get(), 0);
}
