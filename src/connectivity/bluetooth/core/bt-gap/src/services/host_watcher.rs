// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    async_helpers::hanging_get::server as hanging_get,
    fidl_fuchsia_bluetooth as btfidl,
    fidl_fuchsia_bluetooth_sys::{self as sys, HostWatcherRequest, HostWatcherRequestStream},
    fuchsia_async as fasync,
    fuchsia_bluetooth::types::{
        host_info::HostInfo,
        HostId,
    },
    fuchsia_syslog::fx_log_warn,
    fuchsia_zircon as zx,
    futures::{StreamExt, Stream},
    std::{
        collections::{HashMap, HashSet},
        mem,
    },
};

use crate::{host_dispatcher::*};

pub async fn run(
    hd: HostDispatcher,
    mut stream: HostWatcherRequestStream,
) -> Result<(), Error> {
    let mut watch_hosts_subscriber = hd.watch_hosts().await;
    while let Some(event) = stream.next().await {
        handler(hd.clone(), &mut watch_hosts_subscriber, event?).await?;
    }
    Ok(())
}

async fn handler(
    hd: HostDispatcher,
    watch_hosts_subscriber: &mut hanging_get::Subscriber<sys::HostWatcherWatchResponder>,
    request: HostWatcherRequest,
) -> Result<(), Error> {
    match request {
        HostWatcherRequest::Watch{ responder } => {
            match watch_hosts_subscriber.register(responder).await {
                Ok(()) => Ok(()),
                // TODO(nickpollard): respond with error if error?
                Err(e) => Err(format_err!("Could not watch hosts")),
            }
        }
        HostWatcherRequest::SetActive{ id, responder } => {
            let mut result = hd.set_active_host(id.into());
            let mut result = result.map_err(|_| zx::Status::NOT_FOUND.into_raw());
            responder.send(&mut result);
            Ok(())
        }
    }
}

// Written as an associated function in order to match the signature of the HangingGet
pub fn observe_hosts(new_hosts: &Vec<HostInfo>, responder: sys::HostWatcherWatchResponder) {
    let mut hosts = new_hosts.into_iter().map(|host| sys::HostInfo::from(host));
    if let Err(err) = responder.send(&mut hosts) {
        fx_log_warn!("Unable to respond to host_watcher watch hanging get: {:?}", err);
    }
}
