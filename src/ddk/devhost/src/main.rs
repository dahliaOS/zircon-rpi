// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]
#![allow(warnings)]

use {
    failure::{format_err, Error},
    fuchsia_syslog::{self as syslog},
    log::*,
};
mod klog;

fn main() -> Result<(), Error> {
    klog::KernelLogger::init().expect("Failed to initialize logger");

    info!("Initialized a devhost");

    let root_channel = fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::User0.into()).ok_or(format_err!("missing startup handle"))?;
    let root_resource = fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::Resource.into()).ok_or(format_err!("missing root resource handle"))?;
    Ok(())
}
