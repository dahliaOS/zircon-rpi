// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![no_std]
#![feature(alloc)]

pub extern crate fuchsia_ddk_sys as sys;

extern crate alloc;

use {
    alloc::prelude::*,
    core::slice,
    fuchsia_ddk_sys::zx_device_t,
    fuchsia_zircon as zx,
    core::marker::PhantomData,
};


/// OpaqueCtx is for parent devices where the type is unknown
/// since it was not created by this driver.
// TODO(bwb): Is there a way to make this only constructed by this crate
// with a hidden type? Maybe not make pub after proc_macro?
pub struct OpaqueCtx(());

#[inline]
unsafe fn strlen(p: *const libc::c_char) -> usize {
    let mut n = 0;
    while *p.offset(n as isize) != 0 {
        n += 1;
    }
    n
}

#[derive(Debug)]
pub struct Device<T> {
    device: *mut zx_device_t,
    ctx_type: PhantomData<T>,
}

impl<T> Device<T> {
    /// Construct a Device from a raw pointer
    pub unsafe fn from_raw_ptr(raw: *mut zx_device_t) -> Device<OpaqueCtx> {
        Device{
            device: raw,
            ctx_type: PhantomData,
        }
    }

    /// TODO: Should this consume self?
    pub fn get_ptr(&self) -> *mut zx_device_t {
        self.device as *mut _
    }

    /// Returns the name of the device
    pub fn get_name(&self) -> &str {
        unsafe {
            let name_ptr = sys::device_get_name(self.device);
            let len = strlen(name_ptr);
            let ptr = name_ptr as *const u8;
            let slice = slice::from_raw_parts(ptr, len); // knock off the null byte
            core::str::from_utf8(slice).unwrap()
        }
    }

    /// Creates a child device and adds it to the devmgr. No context is provided
    /// so nothing is allocated.
    pub fn add_device(self, name: String) -> Result<Device<()>, zx::Status> {
        // zero sized types don't allocate anything
        self.add_device_with_context(name, Box::new(()))
    }

    /// Creates a child device and adds it to the devmgr
    /// TODO(bwb): Think about lifetimes. Consumes Device to prevent calls on the parent device
    /// after a potential release or failure. Might need to rethink if parent still needed in bind calls.
    pub fn add_device_with_context<U>(self, name: String, context: Box<U>) -> Result<Device<U>, zx::Status> {
        let mut name_vec: Vec<u8> = name.clone().into();
        name_vec.reserve_exact(1);
        name_vec.push(0);

        // device_add_args_t values are copied, so device_add_args_t can be stack allocated.
        let mut args = sys::device_add_args_t {
            name: name_vec.as_ptr(),
            version: sys::DEVICE_ADD_ARGS_VERSION,
            ops: unsafe { &mut DEVICE_OPS } as *mut _,
            ctx: Box::leak(context) as *mut _ as *mut libc::c_void,
            props: core::ptr::null_mut(),
            flags: 1,
            prop_count: 0,
            proto_id: 0,
            proto_ops: core::ptr::null_mut(),
            proxy_args: core::ptr::null_mut(),
            client_remote: 0, //handle
        };

        let mut out: zx_device_t = zx_device_t::default();
        let mut out_ptr = &mut out as *mut _;
        let resp = unsafe {
            // TODO(bwb) think about validating out_ptr or trust invariants of call?
            sys::device_add_from_driver(
                sys::__zircon_driver_rec__.driver,
                self.get_ptr(),
                &mut args,
                &mut out_ptr,
            )
        };
        zx::Status::ok(resp).map(|_| {
            Device {
                device: out_ptr,
                ctx_type: PhantomData
            }
        })
    }
}

#[no_mangle]
pub static mut DEVICE_OPS: sys::zx_protocol_device_t = sys::zx_protocol_device_t {
    version: sys::DEVICE_OPS_VERSION,
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
