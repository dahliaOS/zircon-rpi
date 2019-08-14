// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file

use {
    crate::dlfcn,
    failure::Error,
    fuchsia_ddk_sys as sys,
    fuchsia_zircon::{self as zx, AsHandleRef},
    log::*,
    std::collections::HashSet,
    std::ffi::CString,
    std::hash::{Hash, Hasher},
    std::rc::Rc,
};

/// All drivers currently loaded into this devhost
// TODO(bwb) make newtype?
pub type DriverSet = HashSet<Rc<Driver>>;

#[derive(Debug)]
pub struct Driver {
    pub path: String,
    pub name: String,
    /// Internal ops table for drivers
    ops: *const *const sys::zx_driver_ops_t,
    /// Driver Context
    ctx: *mut libc::c_void,
}

// A Driver is currently considered the same as another if it has the same path
impl Hash for Driver {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.path.hash(state);
    }
}

// PartialEq/Eq must be the same as hashing
impl PartialEq for Driver {
    fn eq(&self, other: &Self) -> bool {
        self.path == other.path
    }
}
impl Eq for Driver {}

#[repr(C)]
#[derive(Debug)]
struct DriverNoteHeader {
    namesz: u32,
    descsz: u32,
    ty: u32,
    name: [u8; 8],
}

#[repr(C)]
#[derive(Debug)]
struct DriverNotePayload {
    flags: u32,
    bindcount: u32,
    _reserved0: u32,
    name: [u8; 32],
    vendor: [u8; 16],
    version: [u8; 16],
}

#[repr(C)]
#[derive(Debug)]
struct DriverNote {
    pub header: DriverNoteHeader,
    pub payload: DriverNotePayload,
}

impl Driver {
    pub fn new(path: String, vmo: zx::Vmo) -> Result<Rc<Self>, Error> {
        let dyn_lib = unsafe {
            // TODO(bwb) clean up dlfcn, don't need most of it
            let dyn_lib = dlfcn::dlopen_vmo(vmo.raw_handle(), libc::RTLD_NOW);
            if dyn_lib.is_null() {
                error!("Could not open driver VMO");
                return Err(zx::Status::IO_INVALID.into());
            }
            dyn_lib
        };

        let driver_note = unsafe {
            let c_string = CString::new("__zircon_driver_note__")?;
            let bytes = c_string.as_bytes_with_nul();
            let driver_note_raw = libc::dlsym(dyn_lib, bytes.as_ptr() as *const _);
            if driver_note_raw.is_null() {
                error!("Missing __zircon_driver_note__ for {}", path);
                return Err(zx::Status::IO_INVALID.into());
            }
            let driver_note: *const DriverNote = std::mem::transmute(driver_note_raw);
            driver_note
        };

        let ops = unsafe {
            let c_string = CString::new("__zircon_driver_ops__")?;
            let bytes = c_string.as_bytes_with_nul();
            let driver_ops_raw = libc::dlsym(dyn_lib, bytes.as_ptr() as *const _);
            if driver_ops_raw.is_null() {
                error!("Missing __zircon_driver_ops__ for {}", path);
                return Err(zx::Status::IO_INVALID.into());
            }
            let driver_ops: *const *const sys::zx_driver_ops_t =
                std::mem::transmute(driver_ops_raw);
            driver_ops
        };

        // context that gets passed around in the driver.
        // TODO(bwb) make const? It's mutated by the driver but ddk never touches
        let mut ctx: *mut libc::c_void = std::ptr::null_mut();

        // If initialize is defined in the driver's ops table, run it
        unsafe {
            if let Some(init) = (**ops).init {
                // calling init function
                let resp = (init)(&mut ctx as *mut _);
                zx::Status::ok(resp)?;
            }
        }

        let name = unsafe { String::from_utf8((*driver_note).payload.name.to_vec())? };
        Ok(Rc::new(Driver { path, name, ops, ctx }))
    }

    /// Returns a reference to the drivers ops tables
    pub fn get_ops_table(&self) -> &sys::zx_driver_ops_t {
        unsafe {
            if self.ops.is_null() {
                error!("Bad ops table access");
            }
            &**self.ops
        }
    }
}
