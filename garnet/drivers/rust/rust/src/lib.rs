// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(unused)]
#![allow(non_camel_case_types,non_snake_case)]
//#![no_std]

use {
    banjo_ddk_protocol_platform_device::*,
    banjo_ddk_protocol_gpio::*,
    fuchsia_ddk::{OpaqueCtx, Device, Ctx, DeviceOps}, //, Op, ProtocolDevice},
    fuchsia_ddk::sys::*,
    fuchsia_zircon::{self as zx},
    std::marker::PhantomData,
};

// TODO make this a fancy macro
pub static DEVICE_OPS: zx_protocol_device_t = zx_protocol_device_t {
    version: DEVICE_OPS_VERSION,
    close:  None,
    ioctl:  None,
    message:  None,
    get_protocol: None,
    get_size: None,
    open:  None,
    open_at:  None,
    read:  None,
    release:  None,
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

#[derive(Default)]
pub struct Gpio {
    gpios: Vec<GpioProtocol>,
}

impl DeviceOps for Gpio {
    fn unbind(device: &Device<Gpio>) {
        device.remove();
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
        irq_count:  0,
        gpio_count: 0,
        i2c_channel_count: 0,
        clk_count: 0,
        bti_count: 0,
        smc_count: 0,
        metadata_count: 0,
        reserved: core::ptr::null_mut(),
        name: core::ptr::null_mut(),
    };

    let mut ctx = Ctx::new(Gpio::default());

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
            platform_device.get_protocol(ZX_PROTOCOL_GPIO, i as u32,
                                         &mut ctx.gpios[i] as *mut _ as *mut libc::c_void,
                                         std::mem::size_of::<GpioProtocol>(),
                                         &mut actual);
        }
        let status = unsafe { ctx.gpios[i].config_out(0) };
        eprintln!("[rust_example] status of setting config_out: {}", status);

    }

    let example_device = parent_device.add_device_with_context(String::from("rust-gpio-example"), ctx, &DEVICE_OPS);

    eprintln!("[rust_example] nothing crashed on device add!");

    Ok(())
}
