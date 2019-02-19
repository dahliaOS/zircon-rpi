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

pub use fuchsia_ddk_macro::bind_entry_point;

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
    real_ctx: Box<T>,
}

impl<T> Ctx<T> {
    pub fn new(context: T) -> Self {
        Ctx {
            device: Device::default(),
            real_ctx: Box::new(context)
        }
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
    pub fn add_device_with_context<U>(self, name: String, context: Ctx<U>, device_ops: &sys::zx_protocol_device_t) -> Result<Device<U>, zx::Status> {
        let mut name_vec: Vec<u8> = name.clone().into();
        name_vec.reserve_exact(1);
        name_vec.push(0);

        let ctx = Box::new(context);
        let ctx_ptr = Box::into_raw(ctx);

        // device_add_args_t values are copied, so device_add_args_t can be stack allocated.
        let mut args = sys::device_add_args_t {
            name: name_vec.as_ptr() as *mut libc::c_char,
            version: sys::DEVICE_ADD_ARGS_VERSION,
            ops: device_ops as *const _,
            ctx: ctx_ptr as *mut _ as *mut libc::c_void,
            props: core::ptr::null_mut(),
            flags: 1,
            prop_count: 0,
            proto_id: 0,
            proto_ops: core::ptr::null_mut(),
            proxy_args: core::ptr::null_mut(),
            client_remote: 0, //handle
        };

        let mut out_ptr = unsafe { (*ctx_ptr).device.device };
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

//#[doc(hidden)]
//#[repr(transparent)]
//pub struct Op<Ctx, Args, Return> {
//    #[doc(hidden)]
//    pub ptr: *const libc::c_void,
//    #[doc(hidden)]
//    pub _marker: PhantomData<(Ctx, Args, Return)>,
//}
//
//unsafe impl<Ctx, Args, Return> Sync for Op<Ctx, Args, Return> { }
//
//#[repr(transparent)]
//pub struct ProtocolDevice<T> {
//    // TODO deref into ops instead of pub
//    pub ops: sys::zx_protocol_device_t,
//    phantom: PhantomData<T>,
//}
//
//unsafe impl<T> Sync for ProtocolDevice<T> { }
//
//impl<Ctx> ProtocolDevice<Ctx> {
//    pub const fn get_protocol(self, get_protocol: Op<Ctx, u32, *mut libc::c_void>) -> Self {
//        Self {
//            ops: sys::zx_protocol_device_t {
//                get_protocol: get_protocol.ptr as *const libc::c_void,
//                version: self.ops.version,
//                close: self.ops.close,
//                ioctl: self.ops.ioctl,
//                message: self.ops.message,
//                get_size: self.ops.get_size,
//                open:   self.ops.open,
//                open_at: self.ops.open_at,
//                read:   self.ops.read,
//                release: self.ops.release,
//                resume: self.ops.resume,
//                rxrpc:  self.ops.rxrpc,
//                suspend: self.ops.suspend,
//                unbind: self.ops.unbind,
//                write:  self.ops.write,
//            },
//            phantom: PhantomData,
//        }
//    }
//
//    pub const fn unitialized() -> Self {
//        Self {
//            ops: sys::zx_protocol_device_t {
//                version: sys::DEVICE_OPS_VERSION,
//                close: core::ptr::null_mut(),
//                ioctl: core::ptr::null_mut(),
//                message: core::ptr::null_mut(),
//                get_protocol: core::ptr::null_mut(),
//                get_size: core::ptr::null_mut(),
//                open: core::ptr::null_mut(),
//                open_at: core::ptr::null_mut(),
//                read: core::ptr::null_mut(),
//                release: core::ptr::null_mut(),
//                resume: core::ptr::null_mut(),
//                rxrpc: core::ptr::null_mut(),
//                suspend: core::ptr::null_mut(),
//                unbind: core::ptr::null_mut(),
//                write: core::ptr::null_mut(),
//            },
//            phantom: PhantomData,
//        }
//    }
//}
