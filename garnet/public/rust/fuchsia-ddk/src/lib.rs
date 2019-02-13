// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub extern crate fuchsia_ddk_sys as sys;

use {
    fuchsia_ddk_sys::zx_device_t,
    std::ffi::{CStr, CString},
};

pub mod protocols;

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
    pub fn get_name(&self) -> &CStr {
        unsafe {
            let resp = sys::device_get_name(self.0);
            // TODO validate
            CStr::from_ptr(resp)
        }
    }

    /// Creates a child device and adds it to the devmgr
    /// TODO(bwb): rename to add_child?
    pub fn add_device<T: Into<Vec<u8>>>(&self, name: T) -> Result<Device, ()> {
        let name_cstring = CString::new(name).unwrap(); // TODO ?

        // device_add_args_t values are copied, so device_add_args_t can be stack allocated.
        let mut args = sys::device_add_args_t {
                name: name_cstring.as_ptr(),
                version: 0x96a64134d56e88e3, // DEVICE_ADD_ARGS_VERSION,
                ops: unsafe { &mut DEVICE_OPS } as *mut _, // std::ptr::null_mut(),

                ctx: std::ptr::null_mut(),
                props: std::ptr::null_mut(),
                flags: 1,
                prop_count: 0,
                proto_id: 0,
                proto_ops: std::ptr::null_mut(),
                proxy_args: std::ptr::null_mut(),
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

