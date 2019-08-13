// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]
#![allow(warnings)]

use {
    failure::{format_err, Error},
    fidl::endpoints::RequestStream,
    fidl_fuchsia_device_manager::{DevhostControllerRequest, DevhostControllerRequestStream},
    fuchsia_async as fasync,
    fuchsia_zircon::{self as zx},
    futures::TryStreamExt,
    log::*,
    device::Device,
    driver::{DriverSet, Driver},
};

/// devhost drivers
mod driver;
/// devhost device
mod device;
/// devhost currently uses the kernel log
mod klog;
/// dlfcn is a bindgen'd wrapper for the custom musl implementation
/// on fuchsia which allows access to dlopen_vmo
/// TODO(bwb): remove when bind rules are no longer in elf notes
mod dlfcn;

/// Start connectionting to the Device Coordinator (devcoordantor). This is the entry point
/// for all interaction between the two primary parts of the DDk.
async fn connect_devcoordinator(
    channel: fasync::Channel,
    mut drivers: DriverSet,
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

                let driver = Driver::new(driver_path, driver)?;
                drivers.insert(driver.clone());

                let device = Device::new("WHAT SHOULD THIS BE", local_device_id);
                device.connect_controller(rpc, None).await?;


                // TODO check if the driver is already in vector
                // for driver in drivers {

                //info!("Initializing a Device with {:?}", driver);
                //info!("pushed: {:?}", driver);
                //device_controller::connect(rpc, local_device_id, Some(driver)).await?;
                //info!("inited");
            }
            DevhostControllerRequest::CreateDeviceStub {
                rpc,
                protocol_id: _,
                local_device_id,
                control_handle: _,
            } => {
                info!("Created Device Stub {}", local_device_id);
                let device = Device::new("proxy", local_device_id);
//                device.set_protocol

                device.connect_controller(rpc, None).await?;
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

    let devhost_drivers: DriverSet = Default::default();
    let status = connect_devcoordinator(fasync::Channel::from_channel(root_channel)?, devhost_drivers).await;
    info!("Device Coordination terminated with {:?}", status);
    Ok(())
}
