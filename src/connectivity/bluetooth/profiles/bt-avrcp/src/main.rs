// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![feature(async_await, await_macro, mpsc_select, slice_index_methods)]
#![recursion_limit = "256"]

use {
    failure::Error,
    fuchsia_async as fasync, fuchsia_syslog,
    futures::{channel::mpsc, join},
};

use crate::peer::PeerManager;
use crate::profile::AvrcpProfile;

mod packets;
mod peer;
mod profile;
mod service;
mod types;

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["avrcp", "avctp"]).expect("Can't init logger");
    fuchsia_syslog::set_verbosity(2);

    // Begin searching for AVRCP target/controller SDP records on newly connected remote peers
    // and register our AVRCP service with the BrEdr profile service.
    let profile_svc = await!(AvrcpProfile::connect_and_register_service())
        .expect("Unable to connect to BrEdr Profile Service");

    // Create a channel that peer manager will receive requests for peer controllers from the
    // FIDL service runner
    let (client_sender, peer_controller_request_receiver) = mpsc::channel(512);

    let mut peer_manager = PeerManager::new(profile_svc, peer_controller_request_receiver)
        .expect("Unable to create Peer Manager");

    let service_fut =
        service::run_service(client_sender).expect("Unable to start AVRCP FIDL service");

    let manager_fut = peer_manager.run();

    // Pump both the fidl service and the peer manager. Neither one should complete unless
    // we are shutting down or there is an unrecoverable error.
    join!(service_fut, manager_fut);

    Ok(())
}
