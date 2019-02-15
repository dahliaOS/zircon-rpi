// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(unused)]
#![allow(non_camel_case_types,non_snake_case)]
//#![no_std]

use {
    banjo_ddk_protocol_platform_device::*,
    banjo_ddk_protocol_gpio::*,
//    std::ffi::CString,
    fuchsia_ddk::Device,
    fuchsia_ddk::sys::*,
    fuchsia_zircon::{
        self as zx,
        sys::{zx_status_t, ZX_ERR_NOT_SUPPORTED, ZX_OK},
    },
};

#[no_mangle]
pub extern "C" fn rust_example_bind(ctx: *mut libc::c_void, parent_device: *mut zx_device_t) -> zx_status_t {
    eprintln!("[rust_example] Binding driver");
    let parent_device = unsafe {
        Device::from_raw_ptr(parent_device)
    };

    eprintln!("[rust_example] parent device name: {}", parent_device.get_name());

    let platform_device = PDevProtocol::from_device(&parent_device).unwrap();
    eprintln!("[rust_example] no crash getting platform device protocol");

    let mut info = pdev_device_info_t {
        vid: 0,
        pid: 0,
        did: 0,
        mmio_count: 0,
        irq_count:  0,
        gpio_count: 0,
        i2c_channel_count: 0,
        clk_count: 0,
        bti_count: 0,
        smc_count: 0,
        metadata_count: 0,
        reserved: core::ptr::null_mut(),
        name: core::ptr::null_mut(),
    };

    let resp = unsafe { platform_device.get_device_info(&mut info) };
    eprintln!("[rust_example] number of gpios: {}", info.gpio_count);

    for i in 0..info.gpio_count {
        eprintln!("[rust_example] working on gpio: {}", i);
        let mut actual = 0;
        let mut gpio = GpioProtocol::default();
        let gpio_size = std::mem::size_of::<GpioProtocol>();
        unsafe {
            platform_device.get_protocol(ZX_PROTOCOL_GPIO, i, &mut gpio as *mut _ as *mut libc::c_void, gpio_size, &mut actual);
        }
        let status = unsafe { gpio.config_out(0) };
        eprintln!("[rust_example] status of setting config_out: {}", status);
    }

    //let example_device = parent_device.add_device(args);
    let example_device = parent_device.add_device(String::from("rust-gpio-example"));

    eprintln!("[rust_example] nothing crashed on device add!");

    ZX_OK
}
