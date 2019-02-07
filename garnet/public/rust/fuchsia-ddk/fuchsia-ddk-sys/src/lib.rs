// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(non_camel_case_types,non_snake_case)]
#![deny(warnings)]

extern crate fuchsia_zircon as zircon;

use std::ffi::{CStr, CString};
pub mod protocols;

// use zircon::sys as sys;
// use std::os::raw::c_char;

#[no_mangle]
pub static mut DEVICE_OPS: zx_protocol_device_t =  zx_protocol_device_t {
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

// bindgeny things

#[derive(Debug)]
#[repr(transparent)]
pub struct Device(*mut zx_device_t); // sys:: prefix this?

// TODO check this dear god
// unsafe impl Send for Device {}

impl Device {
    // TODO(bwb): Unowned stuff like channel does?
    pub unsafe fn from_raw_ptr(raw: *mut zx_device_t) -> Device {
        Device(raw)
    }

    pub fn get_ptr(&self) -> *mut zx_device_t {
        self.0 as *mut _
    }

    pub fn add_device<T: Into<Vec<u8>>>(&self, name: T, ) -> Result<Device, ()> {
        let name_cstring = CString::new(name).unwrap(); // TODO ?

        // device_add_args_t values are copied, so device_add_args_t can be stack allocated.
        let mut args = device_add_args_t {
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
            let resp = device_add_from_driver(__zircon_driver_rec__.driver, self.get_ptr(), &mut args, &mut out_ptr);
            if resp != fuchsia_zircon::sys::ZX_OK {
                return Err(()); // TODO actual errors
            }
            Ok(Device(out_ptr))
        }
    }

    pub fn get_name(&self) -> &CStr {
        unsafe {
            let resp = device_get_name(self.0);
            // TODO validate
            CStr::from_ptr(resp)
        }
    }
}

//pub struct DeviceAddArg(device_a
//
//impl DeviceAddArg {
//    pub fn new() -> Self {
//
//    }
//
//}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct device_add_args_t {
    pub version: u64,
    pub name: *const ::std::os::raw::c_char,
    pub ctx: *mut ::std::os::raw::c_void,
    pub ops: *mut zx_protocol_device_t,
    pub props: *mut zx_device_prop_t,
    pub prop_count: u32,
    pub proto_id: u32,
    pub proto_ops: *mut ::std::os::raw::c_void,
    pub proxy_args: *const ::std::os::raw::c_char,
    pub flags: u32,
    pub client_remote: zircon::sys::zx_handle_t,
}


//// References to Zircon DDK's driver.h
//
//// Copied from fuchsia-zircon-sys.




// driver below

#[link(name = "ddk")]
extern "C" {
    pub static __zircon_driver_rec__: zx_driver_rec_t;

    pub fn device_get_name(dev: *mut zx_device_t) -> *const ::std::os::raw::c_char;
    pub fn device_get_parent(dev: *mut zx_device_t) -> *mut zx_device_t;
    pub fn device_get_protocol(dev: *const zx_device_t, proto_id: u32, protocol: *mut ::std::os::raw::c_void,) -> zircon::sys::zx_status_t;
    pub fn device_add_from_driver(drv: *mut zx_driver_t, parent: *mut zx_device_t, args: *mut device_add_args_t, out: *mut *mut zx_device_t) -> zircon::sys::zx_status_t;
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_driver_rec_t {
    pub ops: *const zx_driver_ops_t,
    pub driver: *mut zx_driver_t,
    pub log_flags: u32,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_protocol_device_t {
    pub version: u64,
    pub get_protocol: ::std::option::Option<
        unsafe extern "C" fn(
            ctx: *mut ::std::os::raw::c_void,
            proto_id: u32,
            protocol: *mut ::std::os::raw::c_void,
        ) -> zircon::sys::zx_status_t,
    >,
    pub open: ::std::option::Option<
        unsafe extern "C" fn(
            ctx: *mut ::std::os::raw::c_void,
            dev_out: *mut *mut zx_device_t,
            flags: u32,
        ) -> zircon::sys::zx_status_t,
    >,
    pub open_at: ::std::option::Option<
        unsafe extern "C" fn(
            ctx: *mut ::std::os::raw::c_void,
            dev_out: *mut *mut zx_device_t,
            path: *const ::std::os::raw::c_char,
            flags: u32,
        ) -> zircon::sys::zx_status_t,
    >,
    pub close: ::std::option::Option<
        unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void, flags: u32) -> zircon::sys::zx_status_t,
    >,
    pub unbind: ::std::option::Option<unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void)>,
    pub release: ::std::option::Option<unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void)>,
    pub read: ::std::option::Option<
        unsafe extern "C" fn(
            ctx: *mut ::std::os::raw::c_void,
            buf: *mut ::std::os::raw::c_void,
            count: usize,
            off: zircon::sys::zx_off_t,
            actual: *mut usize,
        ) -> zircon::sys::zx_status_t,
    >,
    pub write: ::std::option::Option<
        unsafe extern "C" fn(
            ctx: *mut ::std::os::raw::c_void,
            buf: *const ::std::os::raw::c_void,
            count: usize,
            off: zircon::sys::zx_off_t,
            actual: *mut usize,
        ) -> zircon::sys::zx_status_t,
    >,
    pub get_size:
        ::std::option::Option<unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void) -> zircon::sys::zx_off_t>,
    pub ioctl: ::std::option::Option<
        unsafe extern "C" fn(
            ctx: *mut ::std::os::raw::c_void,
            op: u32,
            in_buf: *const ::std::os::raw::c_void,
            in_len: usize,
            out_buf: *mut ::std::os::raw::c_void,
            out_len: usize,
            out_actual: *mut usize,
        ) -> zircon::sys::zx_status_t,
    >,
    pub suspend: ::std::option::Option<
        unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void, flags: u32) -> zircon::sys::zx_status_t,
    >,
    pub resume: ::std::option::Option<
        unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void, flags: u32) -> zircon::sys::zx_status_t,
    >,
    pub rxrpc: ::std::option::Option<
        unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void, channel: zircon::sys::zx_handle_t) -> zircon::sys::zx_status_t,
    >,
    pub message: ::std::option::Option<
        unsafe extern "C" fn(
            ctx: *mut ::std::os::raw::c_void,
            msg: *mut fidl_msg_t,
            txn: *mut fidl_txn_t,
        ) -> zircon::sys::zx_status_t,
    >,
}
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct fidl_msg {
    _unused: [u8; 0],
}
pub type fidl_msg_t = fidl_msg;
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct fidl_txn {
    _unused: [u8; 0],
}
pub type fidl_txn_t = fidl_txn;


#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_driver_ops_t {
    pub version: u64,
    pub init: ::std::option::Option<
        unsafe extern "C" fn(out_ctx: *mut *mut ::std::os::raw::c_void) -> zircon::sys::zx_status_t,
    >,
    pub bind: ::std::option::Option<
        unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void, device: *mut zx_device_t)
            -> zircon::sys::zx_status_t,
    >,
    pub create: ::std::option::Option<
        unsafe extern "C" fn(
            ctx: *mut ::std::os::raw::c_void,
            parent: *mut zx_device_t,
            name: *const ::std::os::raw::c_char,
            args: *const ::std::os::raw::c_char,
            rpc_channel: zircon::sys::zx_handle_t,
        ) -> zircon::sys::zx_status_t,
    >,
    pub release: ::std::option::Option<unsafe extern "C" fn(ctx: *mut ::std::os::raw::c_void)>,
}

#[repr(C)]
#[derive(Default, Debug, Copy, Clone)]
pub struct zx_device_t {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_driver_t {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_device_prop_t {
    pub id: u16,
    pub reserved: u16,
    pub value: u32,
}

// #[repr(C)]
// #[derive(Debug, Copy, Clone)]
// pub struct device_add_args_t {
//     pub version: u64,
//     pub name: *const ::std::os::raw::c_char,
//     pub ctx: *mut ::std::os::raw::c_void,
//     pub ops: *mut zx_protocol_device_t,
//     pub props: *mut zx_device_prop_t,
//     pub prop_count: u32,
//     pub proto_id: u32,
//     pub proto_ops: *mut ::std::os::raw::c_void,
//     pub proxy_args: *const ::std::os::raw::c_char,
//     pub flags: u32,
//     pub client_remote: zircon::sys::zx_handle_t,
// }


//// References to Zircon DDK's driver.h
//
//// Copied from fuchsia-zircon-sys.
//macro_rules! multiconst {
//    ($typename:ident, [$($rawname:ident = $value:expr;)*]) => {
//        $(
//            pub const $rawname: $typename = $value;
//        )*
//    }
//}
//
//// Opaque structs
//#[repr(u8)]
//pub enum zx_device_t {
//    variant1,
//}
//
//#[repr(u8)]
//pub enum zx_device_prop_t {
//    variant1,
//}
//
//#[repr(u8)]
//pub enum zx_driver_t {
//    variant1,
//}
//
//#[repr(C)]
//pub struct zx_driver_rec_t {
//    pub ops: *const zx_driver_ops_t,
//    pub driver: *mut zx_driver_t,
//    pub log_flags: u32,
//}
//
//pub const ZX_DEVICE_NAME_MAX: usize = 31;
//
//#[repr(C)]
//pub struct list_node_t {
//    pub prev: *mut list_node_t,
