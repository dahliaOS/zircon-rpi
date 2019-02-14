// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(unused)]
#![allow(non_camel_case_types,non_snake_case)]
#![no_std]

use {
    banjo_ddk_protocol_platform_device::*,
 //   banjo_ddk_protocol_serial::*,
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
    //eprintln!("[rust_example] Binding driver");
    let parent_device = unsafe {
        Device::from_raw_ptr(parent_device)
    };

    //eprintln!("[rust_example] {:?}", parent_device.get_name());

    let platform_device = PDevProtocol::from_device(&parent_device);
    //eprintln!("[rust_example] pd no crash");

    //let info = platform_device.get_device_info();
    //eprintln!("[rust_example] pd info: {:?}", info);

    //let example_device = parent_device.add_device(args);
    //let example_device = parent_device.add_device(String::from("rust-example"));

    //eprintln!("[rust_example] nothing crashed on device add!");

    ZX_OK
}
