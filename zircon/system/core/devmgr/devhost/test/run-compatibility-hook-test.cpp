#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/namespace.h>
#include <lib/fdio/spawn.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/fidl-async/bind.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zxtest/zxtest.h>

namespace compat_test {

using driver_integration_test::IsolatedDevmgr;

TEST(DeviceControllerIntegrationTest, RunCompatibilityHookSuccess) {
    IsolatedDevmgr devmgr;
    auto args = IsolatedDevmgr::DefaultArgs();
    args_.driver_search_paths.push_back("/boot/driver");
    args_.driver_search_paths.push_back("/boot/driver/test");
    board_test::DeviceEntry dev = {};
    dev.did = 0;
    dev.vid = PDEV_VID_TEST;
    dev.pid = PDEV_PID_COMPATIBILITY_TEST;
    args_.device_list.push_back(dev);

    zx_status_t status = IsolatedDevmgr::Create(&args_, &devmgr_);
    ASSERT_OK(status);
    fbl::unique_fd fd;
    devmgr_integration_test::RecursiveWaitForFile(devmgr_.devfs_root(),
            "sys/platform/11:03:0/usb-virtual-bus", &fd);
    ASSERT_GT(fd.get(), 0);

    zx::channel device_handle;
    ASSERT_EQ(ZX_OK, fdio_get_service_handle(fd.release(),
               device_handle.reset_and_get_address()));
}
