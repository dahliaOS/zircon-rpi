// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]

use {
    failure::{format_err, Error, ResultExt},
    fidl_fuchsia_virtualization_hardware::{
        VirtioRngRequest, VirtioRngRequestStream,
    },
    fuchsia_component::server,
    fuchsia_async::{self as fasync},
    fuchsia_syslog::{self as syslog, fx_log_warn},
    fuchsia_zircon::{self as zx},
    futures::{StreamExt, TryFutureExt},
    machina_virtio_device::{DeviceBuilder, NotifyEvent, DeviceStream},
    virtio_device::{DescChainBytes},
};


async fn run_virtio_rng(con: VirtioRngRequestStream) -> Result<(), Error> {
    let mut con = DeviceStream::new(con);

    // Expect a Start message as the first thing we receive.
    let (start_info, responder) = match await!(con.next()) {
        Some(Ok(VirtioRngRequest::Start { start_info, responder})) => (start_info, responder),
        _ => return Err(format_err!("Expected Start message.")),
    };

    let (mut device, guest_mem, ready_responder) = await!(DeviceBuilder::new(start_info, || responder.send())?
        .set_event(|e| Ok(NotifyEvent::new(e)))?
        .add_queue(0, true)?
        .wait_for_ready(&mut con))?;

    let mut desc_stream = device.take_stream(0)?;

    // Complete negotiation
    ready_responder.send()?;

    let queue_fut = async move || -> Result<(), Error> {
        while let Some(mut desc_chain) = await!(desc_stream.next()) {
            let mut chain = DescChainBytes::new(desc_chain.iter(&guest_mem))?;

            while let Ok(mem) = chain.next_bytes_writable() {
                unsafe {
                    zx::sys::zx_cprng_draw(mem.as_mut_ptr(), mem.len());
                }
                chain.mark_written(mem.len() as u32);
            }
        }
        Ok(())
    }();
    // Wait for the event notification stream to end, indicating the guest has disconnected.
    await!(futures::future::try_join(device.err_into::<Error>(), queue_fut))?;

    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    syslog::init().expect("Unable to initialize syslog");
    let mut fs = server::ServiceFs::new();
    fs.dir("svc")
        .add_fidl_service(|stream: VirtioRngRequestStream| stream);
    fs.take_and_serve_directory_handle().context("Error starting server")?;
    
    let service_fut = fs.for_each_concurrent(None, |stream| async {
        if let Err(e) = await!(run_virtio_rng(stream)) {
            fx_log_warn!("Error {} running virtio_rng service", e);
        }
    });    

    await!(service_fut);
    Ok(())
}
