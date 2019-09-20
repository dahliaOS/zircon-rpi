This directory contains very simple tests written to check that the data types and
functions in common/vk/vk_app_state.h work properly.

vk_app_state_basic:
  Simple creation / print / destruction of a vk_app_state_t instance,
  without enabling swapchain support.

vk_app_state_swapchain:
  Simple creation / print / destruction of a vk_app_state_t instance,
  *with* swapchain support enabled. However, nothing is displayed and
  the program exist immediately.

vk_app_state_triangle:
  Simple vulkan application that renders a gradient-shaded triangle on
  a window (on the host), or the framebuffer (on a Fuchsia device).
  Implements full presentation support with graphics pipeline operations
  only.

  NOTE: This is a very basic port of the Vulkan Tutorial's "Hello Triangle"
  example.

vk_app_state_transfer:
  A modified version of vk_app_state_triangle that will also transfer a
  host-visible buffer to the image. The buffer content changes on every frame,
  as well as its position. Used to verify that the presentation support
  correctly handles image layout transitions.


To build the tests:

    fx build src/graphics/lib/compute:all_tests
    # Rebuilds everything for host and device.

    fx build vk_app_state_test_triangle
    # Only rebuilds a specific device test program. Note that you cannot
    # install it directly (see below on how to run it).

    fx build host_x64/vk_app_state_test_triangle
    # Only rebuild a specific host test program.


To run the tests:

    out/default/host_x64/vk_app_state_test_triangle
    # Run the test on the host directly.

    (cd out/default && gdb --args host_x64/exe.unstripped/vk_app_state_test_triangle)
    # Run the test on the host inside a debugger. Changing the directory
    # allows the debugger to find the right sources directly.

    fx build updates && fx shell run vk_app_state_test_triangle
    # Install then run the test on the device. Building the "updates" target
    # is necessary to rebuilt the package repository that the device will
    # connect to in order to download the test binary. Without this, just
    # rebuilding the device test on the host has no effect on "fx shell run"
    # command.

    fx shell run vk_app_state_test_triangle
    # Re-run the test if already installed on the device.
