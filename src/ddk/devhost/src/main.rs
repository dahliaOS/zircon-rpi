// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]
#![allow(warnings)]

use {
    failure::{format_err, Error},
    fuchsia_syslog::{self as syslog},
    fuchsia_async as fasync,
    fuchsia_zircon as zx,
    fidl_fuchsia_device_manager::{DevhostControllerRequestStream, DevhostControllerRequest},
    fidl::endpoints::RequestStream,
    futures::TryStreamExt,
    log::*,
};

/// devhost currently uses the kernel log
mod klog;


async fn start_devcoordinator(channel: fasync::Channel) -> Result<(), Error> {
    let mut stream = DevhostControllerRequestStream::from_channel(channel);

    while let Some(request) = stream.try_next().await? {
        match request {
            DevhostControllerRequest::CreateDevice {rpc, driver_path, driver, parent_proxy, proxy_args, local_device_id, control_handle}=> {
                info!("Created Device");
            }
        }
    }
    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    klog::KernelLogger::init().expect("Failed to initialize logger");

    // TODO better startupu message
    info!("Initialized a devhost");

    let root_channel = zx::Channel::from(fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::User0.into())
            .ok_or(format_err!("missing startup handle"))?);

    // TODO(bwb): Nothing uses this? remove?
    let root_resource =
        fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::Resource.into())
            .ok_or(format_err!("missing root resource handle"))?;

    start_devcoordinator(fasync::Channel::from_channel(root_channel)?).await
}
