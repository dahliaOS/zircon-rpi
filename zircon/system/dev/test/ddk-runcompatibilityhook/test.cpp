#include <ddk/platform-defs.h>
#include <fuchsia/device/c/fidl.h>
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

using driver_integration_test::IsolatedDevmgr;

TEST(DeviceControllerIntegrationTest, RunCompatibilityHookSuccess) {
    IsolatedDevmgr devmgr;
    IsolatedDevmgr::Args args;
    args.driver_search_paths.push_back("/boot/driver");
    args.driver_search_paths.push_back("/boot/driver/test");

    board_test::DeviceEntry dev = {};
    dev.vid = PDEV_VID_TEST;
    dev.pid = PDEV_PID_COMPATIBILITY_TEST;
    dev.did = 0;
    args.device_list.push_back(dev);

    zx_status_t status = IsolatedDevmgr::Create(&args, &devmgr);
    ASSERT_OK(status);
    fbl::unique_fd parent_fd, child_fd;
    devmgr_integration_test::RecursiveWaitForFile(devmgr.devfs_root(),
            "sys/platform/11:09:0/compatibility-test", &parent_fd);
    ASSERT_GT(parent_fd.get(), 0);
    //TODO(ravoorir): Why does this fail? Relative path waits do not work.
    /*devmgr_integration_test::RecursiveWaitForFile(parent_fd,
            "compatibility-test-child", &child_fd);
    ASSERT_GT(child_fd.get(), 0);*/

    zx::channel parent_device_handle;
    ASSERT_EQ(ZX_OK, fdio_get_service_handle(parent_fd.release(),
               parent_device_handle.reset_and_get_address()));
    ASSERT_TRUE((parent_device_handle.get() != ZX_HANDLE_INVALID), "");

    const char* child_name = "compatibility-test-child";
    zx_status_t call_status;
    status = fuchsia_device_ControllerRunCompatibilityTests(parent_device_handle.get(), child_name,
                                                            strlen(child_name), &call_status);
    ASSERT_OK(status);
    ASSERT_OK(call_status);
}
