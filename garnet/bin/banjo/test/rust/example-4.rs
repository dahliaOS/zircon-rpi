// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.example4 banjo file

use fuchsia_zircon as zircon;
use fuchsia_ddk_sys as ddk;

// C ABI compat

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct point_t {
    pub x: u64,
}

#[repr(u32)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum enum_t {
    X  = 23,
}

#[repr(C)]
pub struct Interface_ops_t {
    pub func: unsafe extern "C" fn (ctx: *mut u8, x: bool) -> (),

}

#[repr(C)]
pub struct InterfaceProtocol {
    pub ops: *mut Interface_ops_t,
    pub ctx: *mut u8,
}

impl Default for InterfaceProtocol {
    fn default() -> Self {
        InterfaceProtocol {
            ops: core::ptr::null_mut(),
            ctx: core::ptr::null_mut(),
        }
    }
}

impl InterfaceProtocol {
    pub fn from_device(parent_device: &ddk::Device) -> Result<Self, ()> { // TODO error type
        let mut ret = Self::default();
        unsafe {
            let resp = ddk::device_get_protocol(
                parent_device.get_ptr(),
                ddk::protocols::ZX_PROTOCOL_INTERFACE,
                &mut ret as *mut _ as *mut ::std::os::raw::c_void);
            if resp != fuchsia_zircon::sys::ZX_OK {
                return Err(());
            }
            Ok(ret)
        }
    }

        pub fn func(x: bool) -> Result<(), ()> {

        Err(())
    }

}


// idiomatic bindings
