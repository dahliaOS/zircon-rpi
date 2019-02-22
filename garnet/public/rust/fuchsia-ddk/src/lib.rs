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
    core::ops::{DerefMut, Deref},
    core::marker::PhantomData,
};

pub use fuchsia_ddk_macro::{device_ops, bind_entry_point};

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

pub struct Ctx<T> {
    device: Device<T>,
    real_ctx: T,
}

impl<T> Ctx<T> {
    pub fn new(context: T) -> Box<Self> {
        Box::new(
        Ctx {
            device: Device::default(),
            real_ctx: context
        })
    }

    pub fn get_device(&self) -> &Device<T> {
        &self.device
    }

    // TODO make unsafe for now. probably unsound
    pub unsafe fn get_mut_device(&mut self) -> &mut Device<T> {
        &mut self.device
    }
}

impl<T> Deref for Ctx<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.real_ctx
    }
}

impl<T> DerefMut for Ctx<T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.real_ctx
    }
}

pub trait DeviceOps where Self: core::marker::Sized {
//    const OPS: sys::zx_protocol_device_t;
    fn get_device_protocol() -> &'static sys::zx_protocol_device_t;

    //fn get_protocol(_: &Device<Self>) -> () { }
    //fn get_size(_: &Device<Self>) -> () { }
    //fn open(_: &Device<Self>) -> () { }
    //fn close(_: &Device<Self>) -> () { }
    //fn release(_: &Device<Self>) -> () { }
    //fn resume(_: &Device<Self>) -> () { }
    //fn suspend(_: &Device<Self>) -> () { }
    fn unbind(_: &Device<Self>) -> () { }
    fn message(_: &Device<Self>) -> Result<(), zx::Status> {
        Err(zx::Status::NOT_SUPPORTED)
    }
//    fn close(_: &Device<Self>) -> () { }
//    fn release(&mut self);
}

#[derive(Copy, Clone, Debug)]
pub struct Device<T> {
    device: *mut zx_device_t,
    ctx_type: PhantomData<T>,
}

unsafe impl<T: Send + Sync > Send for Device<T> { }

impl<T> Default for Device<T> {
    fn default() -> Self {
        Self {
            device: core::ptr::null_mut(),
            ctx_type: PhantomData,
        }
    }
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

    // TODO(bwb): actually implement
    pub fn remove(&self) -> () {
        // TODO
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

    ///// Creates a child device and adds it to the devmgr. No context is provided
    ///// so nothing is allocated.
    //pub fn add_device(self, name: String) -> Result<Device<()>, zx::Status> {
    //    // zero sized types don't allocate anything
    //    self.add_device_with_context(name, Box::new(()))
    //}

    /// Creates a child device and adds it to the devmgr
    /// TODO(bwb): Think about lifetimes. Consumes Device to prevent calls on the parent device
    /// after a potential release or failure. Might need to rethink if parent still needed in bind calls.
    pub fn add_device_with_context<U: DeviceOps>(self, name: String, protocol_id: u32, mut context: Box<Ctx<U>>) -> Result<Device<U>, zx::Status> {
        let mut name_vec: Vec<u8> = name.clone().into();
        name_vec.reserve_exact(1);
        name_vec.push(0);

        let raw_device_ptr = &mut context.device.device as *mut _;
        let ctx_ptr = Box::into_raw(context);

        // device_add_args_t values are copied, so device_add_args_t can be stack allocated.
        let mut args = sys::device_add_args_t {
            name: name_vec.as_ptr() as *mut libc::c_char,
            version: sys::DEVICE_ADD_ARGS_VERSION,
            ops: U::get_device_protocol() as *const _,
            ctx: ctx_ptr as *mut _ as *mut libc::c_void,
            props: core::ptr::null_mut(),
            flags: 0,
            prop_count: 0,
            proto_id: protocol_id,
            proto_ops: core::ptr::null_mut(),
            proxy_args: core::ptr::null_mut(),
            client_remote: 0, //handle
        };

        let resp = unsafe {
            // TODO(bwb) think about validating out_ptr or trust invariants of call?
            sys::device_add_from_driver(
                sys::__zircon_driver_rec__.driver,
                self.get_ptr(),
                &mut args,
                raw_device_ptr,
            )
        };
        zx::Status::ok(resp).map(|_| {
            Device {
                device: unsafe { *raw_device_ptr },
                ctx_type: PhantomData
            }
        })
    }
}

pub unsafe extern "C" fn unbind_unsafe<T: DeviceOps>(ctx: *mut libc::c_void) {
    // TODO verify unwrap here
    let ctx_ref: &Ctx<T> = &*(ctx as *mut Ctx<T>); // unsafe
    T::unbind(ctx_ref.get_device())
}

pub unsafe extern "C" fn message_unsafe<T: DeviceOps>(
    ctx: *mut libc::c_void,
    _msg: *mut fuchsia_ddk_sys::fidl_msg_t,
    _txn: *mut fuchsia_ddk_sys::fidl_txn_t,
) -> zx::sys::zx_status_t {
    // TODO verify unwrap here
    let ctx_ref: &Ctx<T> = &*(ctx as *mut Ctx<T>);
    let _resp = T::message(ctx_ref.get_device()); // TODO message
    zx::sys::ZX_OK
}
