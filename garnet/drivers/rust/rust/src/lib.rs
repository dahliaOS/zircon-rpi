// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(unused)]
#![allow(non_camel_case_types, non_snake_case)]
//#![no_std]

use {
    banjo_ddk_protocol_gpio::*,
    banjo_ddk_protocol_platform_device::*,
    fuchsia_ddk::sys::*,
    fuchsia_ddk::{Ctx, Device, DeviceOps, OpaqueCtx}, //, Op, ProtocolDevice},
    fuchsia_zircon as zx,
    std::marker::PhantomData,
};

//pub static DEVICE_OPS: zx_protocol_device_t = make_device_ops!(Gpio, [message, unbind, get_protocol]);

// TODO make this a fancy proc_macro
// annotate the whole crate with #![fuchsia_ddk::driver]
// go through anything that is named DeviceOps and build the static ops table
pub static DEVICE_OPS: zx_protocol_device_t = zx_protocol_device_t {
    version: DEVICE_OPS_VERSION,
    close: None,
    ioctl: None,
    message: Some(message_unsafe),
    get_protocol: None,
    get_size: None,
    open: None,
    open_at: None,
    read: None,
    release: None,
    resume: None,
    rxrpc: None,
    suspend: None,
    write: None,
    unbind: Some(unbind_unsafe),
};

pub unsafe extern "C" fn unbind_unsafe(ctx: *mut libc::c_void) {
    // TODO verify unwrap here
    let ctx_ref: &Ctx<Gpio> = unsafe { &*(ctx as *mut Ctx<Gpio>) };
    Gpio::unbind(ctx_ref.get_device())
}

pub unsafe extern "C" fn message_unsafe(
    ctx: *mut libc::c_void,
    msg: *mut fuchsia_ddk::sys::fidl_msg_t,
    txn: *mut fuchsia_ddk::sys::fidl_txn_t,
) -> zx::sys::zx_status_t {
    eprintln!("message_unsafe called!");
    dbg!("message_unsafe called!");
    // TODO verify unwrap here
    let ctx_ref: &Ctx<Gpio> = unsafe { &*(ctx as *mut Ctx<Gpio>) };
    let resp = Gpio::unbind(ctx_ref.get_device()); // TODO message
                                                   //resp
    zx::sys::ZX_OK
}

#[derive(Default)]
pub struct Gpio {
    // TODO(bwb): Check with todd that this is safe to have protocols Send + Sync
    gpios: Vec<GpioProtocol>,
}

impl DeviceOps for Gpio {
    fn unbind(device: &Device<Gpio>) {
        device.remove();
    }
    fn message(device: &Device<Gpio>) -> Result</* TODO(bwb): fidl buffer */ (), zx::Status> {
        dbg!("message called in Gpio!");
        Ok(())
//        device.remove();
    }
}

#[fuchsia_ddk::bind_entry_point]
fn rust_example_bind(parent_device: Device<OpaqueCtx>) -> Result<(), zx::Status> {
    eprintln!("[rust_example] parent device name: {}", parent_device.get_name());

    let platform_device = PDevProtocol::from_device(&parent_device)?;

    eprintln!("[rust_example] no crash getting platform device protocol");

    // TODO(bwb): make pointers derive default of null
    // TODO(bwb): switch autogen of names to be more rusty
    // e.g. PdevDeviceInfo::default()
    let mut info = pdev_device_info_t {
        vid: 0,
        pid: 0,
        did: 0,
        mmio_count: 0,
        irq_count: 0,
        gpio_count: 0,
        i2c_channel_count: 0,
        clk_count: 0,
        bti_count: 0,
        smc_count: 0,
        metadata_count: 0,
        reserved: core::ptr::null_mut(),
        name: core::ptr::null_mut(),
    };

    let mut ctx = Gpio::default();

    // TODO(bwb): Add result type to banjo
    let resp = unsafe { platform_device.get_device_info(&mut info) };
    eprintln!("[rust_example] number of gpios: {}", info.gpio_count);

    for i in 0..info.gpio_count as usize {
        eprintln!("[rust_example] working on gpio number: {}", i);
        ctx.gpios.insert(i, GpioProtocol::default());
        unsafe {
            let mut actual = 0;
            // TODO(bwb): Think about generating _ref bindings after banjo result
            // for more allocation control (e.g. doesn't return Protocol, takes &mut like now)
            platform_device.get_protocol(
                ZX_PROTOCOL_GPIO,
                i as u32,
                &mut ctx.gpios[i] as *mut _ as *mut libc::c_void,
                std::mem::size_of::<GpioProtocol>(),
                &mut actual,
            );
        }
        let status = unsafe { ctx.gpios[i].config_out(0) };
        eprintln!("[rust_example] status of setting config_out: {}", status);
    }

    let example_device = parent_device.add_device_with_context(
        String::from("rust-gpio-example"),
        Ctx::new(std::sync::Arc::new(ctx)),
        &DEVICE_OPS,
    )?;

    //std::thread::spawn(move || {
    //    eprintln!("{}", example_device.get_name());
    //});

    eprintln!("[rust_example] nothing crashed on device add!");

    Ok(())
}
