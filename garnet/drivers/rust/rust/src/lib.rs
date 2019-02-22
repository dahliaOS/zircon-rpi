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

#[derive(Default)]
pub struct Gpio {
    // TODO(bwb): Check with todd that this is safe to have protocols Send + Sync
    gpios: Vec<GpioProtocol>,
}

#[fuchsia_ddk::device_ops]
impl DeviceOps for Gpio {
    fn unbind(device: &Device<Gpio>) {
        //device.remove();
    }
    unsafe fn message(
        device: &Device<Gpio>,
        msg: *mut fidl_msg_t,
        txn: *mut fidl_txn_t,
    ) -> Result<(), zx::Status> {
        dbg!("message called in Gpio!");
        let slice = unsafe {
            std::slice::from_raw_parts((*msg).bytes as *mut u8, (*msg).num_bytes as usize)
        };
        // TODO better error
        let (txn_header, body_bytes) =
            fidl::encoding::decode_transaction_header(slice).map_err(|e| zx::Status::INTERNAL)?;
        let mut handles: Vec<fuchsia_zircon::Handle> = vec![];
        match txn_header.ordinal {
            164944856 | 164944856 => {
                // decode side
                let mut req: (u32) = fidl::encoding::Decodable::new_empty();
                fidl::encoding::Decoder::decode_into(
                    body_bytes,
                    handles.as_mut_slice(),
                    &mut req,
                )
                .map_err(|e| zx::Status::INTERNAL)?;

                // user implemented
                let status: i32 = 0;
                let name = Some("test-wat");
                let mut response = (status, name);

                // encode side
                let header = fidl::encoding::TransactionHeader {
                    tx_id: txn_header.tx_id,
                    flags: 0,
                    ordinal: txn_header.ordinal,
                };

                let mut msg = fidl::encoding::TransactionMessage {
                        header,
                        body: &mut response,
                };

                let (mut bytes, mut handles) = (&mut vec![], &mut vec![]);
                fidl::encoding::Encoder::encode(bytes, handles, &mut msg).map_err(|e| zx::Status::INTERNAL)?;
                if let Some(reply_fn) = (*txn).reply {
                    reply_fn(txn, &fidl_msg_t {
                        bytes: bytes.as_ptr() as *mut libc::c_void,
                        handles: handles.as_ptr() as *mut u32,
                        num_bytes: bytes.len() as u32,
                        num_handles: 0,
                    });
                }
            }
            _ => return Err(zx::Status::NOT_SUPPORTED),
        }
        eprintln!("{:#?}", txn_header);
        Ok(())
    }
}

#[fuchsia_ddk::bind_entry_point]
fn rust_example_bind(parent_device: Device<OpaqueCtx>) -> Result<(), zx::Status> {
    eprintln!("[rust_example] parent device name: {}", parent_device.get_name());
    let platform_device = PDevProtocol::from_device(&parent_device)?;

    let mut info = PdevDeviceInfo::default();
    let mut ctx = Gpio::default();
    // TODO(bwb): Add result type to banjo
    let resp = unsafe { platform_device.get_device_info(&mut info) };
    eprintln!("[rust_example] number of gpios: {}", info.gpio_count);

    for i in 0..info.gpio_count as usize {
        ctx.gpios.insert(i, GpioProtocol::default());
        unsafe {
            let mut actual = 0;
            // TODO(bwb): Think about generating _ref bindings after banjo result
            // for more allocation control (e.g. doesn't return Protocol, takes &mut like now)
            platform_device.get_protocol(
                ZX_PROTOCOL_GPIO,
                i as u32,
                fuchsia_ddk::LibcPtr(&mut ctx.gpios[i] as *mut _ as *mut libc::c_void),
                std::mem::size_of::<GpioProtocol>(),
                &mut actual,
            );
        }
        let status = unsafe { ctx.gpios[i].config_out(0) };
        eprintln!("[rust_example] status of setting config_out: {}", status);
    }

    let example_device = parent_device.add_device_with_context(
        String::from("rust-gpio-light"),
        ZX_PROTOCOL_LIGHT,
        Ctx::new(ctx),
    )?;

    eprintln!("[rust_example] nothing crashed on device add!: {}", example_device.get_name());
    Ok(())
}
