// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![no_std]
#![feature(alloc)]

pub extern crate fuchsia_ddk_sys as sys;

extern crate alloc;

use {
    fuchsia_ddk_sys::zx_device_t,
    alloc::prelude::*,
    core::slice,
};

#[inline]
unsafe fn strlen(p: *const libc::c_char) -> usize {
    let mut n = 0;
    while *p.offset(n as isize) != 0 {
        n += 1;
    }
    n
}

#[derive(Debug)]
#[repr(transparent)]
pub struct Device(*mut zx_device_t); // sys:: prefix this?

impl Device {
    /// Construct a Device from a raw pointer
    pub unsafe fn from_raw_ptr(raw: *mut zx_device_t) -> Device {
        Device(raw)
    }

    /// TODO: Should this consume self?
    pub fn get_ptr(&self) -> *mut zx_device_t {
        self.0 as *mut _
    }

    /// Returns the name of the device
    pub fn get_name(&self) -> &str {
        unsafe {
            let name_ptr = sys::device_get_name(self.0);
            let len = strlen(name_ptr);
            let ptr = name_ptr as *const u8;
            let slice = slice::from_raw_parts(ptr, len); // knock off the null byte
            core::str::from_utf8(slice).unwrap()
        }
    }

    /// Creates a child device and adds it to the devmgr
    /// TODO(bwb): rename to add_child?
    pub fn add_device(&self, name: String) -> Result<Device, ()> {
        let mut name_vec: Vec<u8> = name.clone().into();
        name_vec.reserve_exact(1);
        name_vec.push(0);

        // device_add_args_t values are copied, so device_add_args_t can be stack allocated.
        let mut args = sys::device_add_args_t {
                name: name_vec.as_ptr(),
                version: sys::DEVICE_ADD_ARGS_VERSION,
                ops: unsafe { &mut DEVICE_OPS } as *mut _,
                ctx: core::ptr::null_mut(),
                props: core::ptr::null_mut(),
                flags: 1,
                prop_count: 0,
                proto_id: 0,
                proto_ops: core::ptr::null_mut(),
                proxy_args: core::ptr::null_mut(),
                client_remote: 0 //handle
        };

        let mut out: zx_device_t = zx_device_t::default();
        let mut out_ptr = &mut out as *mut _;
        unsafe {
            let resp = sys::device_add_from_driver(sys::__zircon_driver_rec__.driver, self.get_ptr(), &mut args, &mut out_ptr);
            if resp != fuchsia_zircon::sys::ZX_OK {
                return Err(()); // TODO actual errors
            }
            Ok(Device(out_ptr))
        }
    }
}

#[no_mangle]
pub static mut DEVICE_OPS: sys::zx_protocol_device_t =  sys::zx_protocol_device_t {
    version: 0xc4640f7115d2ee49, // DEVICE_OPS_VERSION,
    close: None,
    get_protocol: None,
    ioctl: None,
    message: None,
    get_size: None,
    open: None,
    open_at: None,
    read: None,
    release: None,
    resume: None,
    rxrpc: None,
    suspend: None,
    unbind: None,
    write: None,
};

