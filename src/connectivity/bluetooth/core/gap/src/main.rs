// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(
    futures_api,
    arbitrary_self_types,
    await_macro,
    async_await
)]
#![allow(unused_imports, dead_code)]

use fidl_fuchsia_bluetooth_control::ControlMarker;
use fidl::endpoints::ServiceMarker;
use failure::{Error, ResultExt};
use fuchsia_app::server::ServicesServer;
use fuchsia_async as fasync;
use futures::future::FutureExt;
use futures::stream::TryStreamExt;

mod actor;
mod adapters;
mod control;
mod discovery;
mod host;
mod host_dispatcher;
mod types;

use crate::host_dispatcher::*;
use crate::host::*;
use crate::actor::{ActorHandle, System};
use crate::adapters::watch_hosts;
use crate::adapters::*;
use crate::control::control_service;

fn main() -> Result<(), Error> {
    let executor = fasync::Executor::new().context("Error creating executor")?;
    let mut system = System::new();
    let dispatcher = system.spawn(HostDispatcher::<FidlHost>::new());
    let d = dispatcher.clone();
    watch_for_hosts(d);

    let system_ = system.clone();

    let d_ = dispatcher.clone();
    let _server =
        ServicesServer::new()
            .add_service((ControlMarker::NAME, move |chan: fasync::Channel| { run_control_service(system_.clone(), d_.clone(), chan)}))
            .start()?;

    system.run(executor)
}

fn run_control_service<H: Host>(system: System, hd: ActorHandle<HostDispatcherMsg<H>>, chan: fasync::Channel) {
    fasync::spawn({
        let dispatcher = hd.clone();
        async {
            await!(control_service(system, dispatcher, chan))
              .unwrap_or_else(|e| eprintln!("Failed to spawn {:?}", e))
    }})
}

fn watch_for_hosts(handle: ActorHandle<HostDispatcherMsg<FidlHost>>) {
    fasync::spawn(
        watch_hosts().try_for_each(
            move |event| send_host_to_dispatcher(handle.clone(), event)
        ).map(|_| ())
    );
}

async fn send_host_to_dispatcher<E>(mut handle: ActorHandle<HostDispatcherMsg<FidlHost>>, event: AdapterEvent) -> Result<(),E> {
    match event {
        AdapterEvent::AdapterAdded(path) => {
            let host = await!(open_host_fidl(path));
            if let Ok(host) = host {
                handle.send(HostDispatcherMsg::AdapterAdded(host.get_info().identifier.into(), host));
            }
        }
        AdapterEvent::AdapterRemoved(path) =>
            handle.send(HostDispatcherMsg::AdapterRemoved(path)),
    }
    Ok(())
}
