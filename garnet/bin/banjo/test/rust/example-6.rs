// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.example6 banjo file

#![allow(unused_imports)]

use fuchsia_zircon as zircon;
use fuchsia_ddk as ddk;


// C ABI compat
pub const x: i32 = 23;





#[repr(C)]
pub struct Hello_ops_t {
    pub say: unsafe extern "C" fn (ctx: *mut u8, req: *mut u8 /*TODO String */, response: *mut u8 /*TODO String */) -> (),

}

#[repr(C)]
pub struct HelloProtocol {
    pub ops: *mut Hello_ops_t,
    pub ctx: *mut u8,
}

impl Default for HelloProtocol {
    fn default() -> Self {
        HelloProtocol {
            ops: core::ptr::null_mut(),
            ctx: core::ptr::null_mut(),
        }
    }
}

impl HelloProtocol {
    pub fn from_device(parent_device: &ddk::Device) -> Result<Self, ()> { // TODO error type
        let mut ret = Self::default();
        unsafe {
            let resp = ddk::sys::device_get_protocol(
                parent_device.get_ptr(),
                ddk::sys::ZX_PROTOCOL_HELLO,
                &mut ret as *mut _ as *mut libc::c_void);
            if resp != fuchsia_zircon::sys::ZX_OK {
                return Err(());
            }
            Ok(ret)
        }
    }

    pub unsafe fn say(&self, req: *mut u8 /*TODO String */, response: *mut u8 /*TODO String */) -> () {
        let no_ret = ((*self.ops).say)(self.ctx, req, response);
        no_ret
    }

}


// idiomatic bindings
