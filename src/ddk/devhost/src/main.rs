// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use {
    failure::{format_err, Error},
    fidl::endpoints::RequestStream,
    fidl_fuchsia_device_manager::{DevhostControllerRequest, DevhostControllerRequestStream},
    fuchsia_async as fasync,
    fuchsia_zircon::{self as zx, AsHandleRef},
    futures::TryStreamExt,
    libc,
    log::*,
    device::Device,
    std::rc::Rc,
};

/// All drivers currently loaded into this devhost
type DevhostDrivers = Vec<Rc<Driver>>;

/// devhost device
mod device;
/// control devices from the devcoordanator
mod device_controller;
/// devhost currently uses the kernel log
mod klog;
/// dlfcn is a bindgen'd wrapper for the custom musl implementation
/// on fuchsia which allows access to dlopen_vmo
/// TODO(bwb): remove when bind rules are no longer in elf notes
mod dlfcn;

#[derive(Debug)]
pub struct Driver {
    pub path: String,
    pub name: String,
}

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
            let dyn_lib = dlfcn::dlopen_vmo(vmo.raw_handle(), libc::RTLD_NOW);
            if dyn_lib.is_null() {
                error!("Could not open driver VMO");
                return Err(zx::Status::IO_INVALID.into());
            }
            dyn_lib
        };
        info!("DYN LIB LOADED: {}", path);
        let driver_note = unsafe {
            let driver_note_raw = libc::dlsym(dyn_lib, "__zircon_driver_note__".as_ptr() as *const i8);
            error!("__zircon_driver_note_raw {:?}", driver_note_raw);
            if driver_note_raw.is_null() {
                error!("Missing __zircon_driver_note__ for {}", path);
                return Err(zx::Status::IO_INVALID.into());
            }
            error!("Found __zircon_driver_note__ for {}", path);
            let driver_note: *const DriverNote = std::mem::transmute(driver_note_raw);
            driver_note
        };

        let name = unsafe { String::from_utf8((*driver_note).payload.name.to_vec())? };
//        info!("DRIVER NOTE: {} ", name);

        // TODO do something with ops
        //unsafe {
        //    let driver_ops_raw = libc::dlsym(dyn_lib, "__zircon_driver_ops__".as_ptr() as *const i8);
        //    if driver_ops_raw.is_null() {
        //        error!("Missing __zircon_driver_ops__");
        //        return Err(zx::Status::IO_INVALID.into());
        //    }
        //}


        Ok(Rc::new(Driver { path, name }))
    }
}

/// Start connectionting to the Device Coordinator (devcoordantor). This is the entry point
/// for all interaction between the two primary parts of the DDk.
async fn connect_devcoordinator(
    channel: fasync::Channel,
    mut drivers: DevhostDrivers,
) -> Result<(), Error> {
    let mut stream = DevhostControllerRequestStream::from_channel(channel);

    while let Some(request) = stream.try_next().await? {
        match request {
            DevhostControllerRequest::CreateDevice {
                rpc,
                driver_path,
                driver,
                parent_proxy: _,
                proxy_args,
                local_device_id,
                control_handle: _,
            } => {
                info!("Create Device {} with {:?}", driver_path, proxy_args);

                // TODO check if the driver is already in vector
                // for driver in drivers {

                let driver = Driver::new(driver_path, driver)?;
                info!("Initializing a Device with {:?}", driver);
                drivers.push(driver.clone());
                info!("pushed: {:?}", driver);
                device_controller::connect(rpc, local_device_id, Some(driver)).await?;
                info!("inited");
            }
            DevhostControllerRequest::CreateDeviceStub {
                rpc,
                protocol_id: _,
                local_device_id,
                control_handle: _,
            } => {
                info!("Created Device Stub {}", local_device_id);
                let _device = Device::new();
                device_controller::connect(rpc, local_device_id, None).await?;
            }
            DevhostControllerRequest::CreateCompositeDevice {
                rpc: _,
                components: _,
                name,
                local_device_id,
                responder: _,
            } => {
                info!("Created Composite Device {} named {}", local_device_id, name);
            }
        }
    }
    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    klog::KernelLogger::init().expect("Failed to initialize access to kernel logger");

    // TODO better startup message
    info!("Initialized a new devhost");

    let root_channel = zx::Channel::from(
        fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::User0.into())
            .ok_or(format_err!("missing startup handle"))?,
    );

    // TODO(bwb): Nothing uses this? remove?
    // let root_resource =
    //     fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::Resource.into())
    //         .ok_or(format_err!("missing root resource handle"))?;

    let devhost_drivers: DevhostDrivers = vec![];
    let status = connect_devcoordinator(fasync::Channel::from_channel(root_channel)?, devhost_drivers).await;
    info!("Device Coordination terminated with {:?}", status);
    Ok(())
}
