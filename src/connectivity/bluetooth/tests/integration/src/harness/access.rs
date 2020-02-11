// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_bluetooth_sys::{AccessMarker, AccessProxy}
    fuchsia_bluetooth::{
        expectation::asynchronous::{ExpectableState, ExpectableStateExt, ExpectationHarness},
        types::{HostId, HostInfo, Peer},
    };
};

#[derive(Clone, Default)]
pub struct AccessState {
    /// Current hosts
    pub hosts: HashMap<HostId, HostInfo>,
    /// Active host identifier
    pub active_host: Option<HostId>,
    /// Remote Peers seen
    pub peers: HashMap<String, Peer>,
}

pub type AccessHarness = ExpectationHarness<AccessState, AccessProxy>;

pub async fn handle_access_events(harness: AccessHarness) -> Result<(), Error> {
    let mut events = harness.aux().take_event_stream();

    // TODO(nickpollard) - repeatedly poll watch_peers
    while let Some(e) = events.try_next().await? {
        match e { };
        harness.notify_state_changed();
    }
    Ok(())
}

pub async fn new_access_harness() -> Result<AccessHarness, Error> {
    let proxy = fuchsia_component::client::connect_to_service::<AccessMarker>()
        .context("Failed to connect to access service")?;

    let access_harness = AccessHarness::new(proxy);

    Ok(access_harness)
}

impl TestHarness for AccessHarness {
    type Env = ();
    type Runner = BoxFuture<'static, Result<(), Error>>;

    fn init() -> BoxFuture<'static, Result<(Self, Self::Env, Self::Runner), Error>> {
        async {
            let harness = new_access_harness().await?;
            let run_access = handle_access_events(harness.clone()).boxed();
            Ok((harness, (), run_access))
        }
        .boxed()
    }
    fn terminate(_env: Self::Env) -> BoxFuture<'static, Result<(), Error>> {
        future::ok(()).boxed()
    }
}
