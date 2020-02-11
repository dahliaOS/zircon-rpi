// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

async fn test_disconnect(access: AccessHarness) -> Result<(), Error> {
    let (_host, mut hci) = activate_fake_host(access.clone(), "bt-hci-integration").await?;

    // Insert a fake peer to test connection and disconnection.
    let peer_address = Address::Random([1, 0, 0, 0, 0, 0]);
    let peer_params = LowEnergyPeerParameters {
        address: Some(peer_address.into()),
        connectable: Some(true),
        advertisement: Some(AdvertisingData {
            data: vec![0x02, 0x01, 0x02], // Flags field set to "general discoverable"
        }),
        scan_response: None,
    };
    let (_peer, remote) = fidl::endpoints::create_proxy()?;
    let _ = hci
        .emulator()
        .add_low_energy_peer(peer_params, remote)
        .await?
        .map_err(|e| format_err!("Failed to register fake peer: {:#?}", e))?;

    // We create a capability to capture the discovery token, and pass it to the access provider
    // Discovery will drop once we drop this token
    let (discovery_token, token_server) = fidl::endpoints::create_proxy()?;
    let fut = access.aux().start_discovery(token_server);
    fut.await?;

    // TODO(nickpollard) - implement the watch_peers runner
    let state = access
        .when_satisfied(
            peer_found_with_address(&peer_address.to_string()),
            timeout(),
        )
        .await?;

    // We can safely unwrap here as this is guarded by the previous expectation
    let peer = state.peers.iter().find(|(_, p)| &p.address == &peer_address.to_string()).unwrap().0;

    let fut = access.aux().connect(peer);
    fut.await?;

    // TODO(nickpollard) - write these expectations
    access.when_satisfied(peer_connected(peer, true), timeout()).await?;
    let fut = access.aux().disconnect(peer);
    fut.await?;

    access.when_satisfied(peer_connected(peer, false), timeout()).await?;

    hci.destroy_and_wait().await?;
    Ok(())
}

/// Run all test cases.
pub fn run_all() -> Result<(), Error> {
    run_suite!("sys.Access", [test_disconnect])
}

pub mod expectation {
    use crate::harness::access::AccessState;
    use fuchsia_bluetooth::{
        types::Peer,
        expectation::Predicate;
    }

    mod peer {
        use super::*;

        pub(crate) fn exists(p: Predicate<Peer>) -> Predicate<AccessState> {
            let msg = format!("peer exists satisfying {}", p.describe());
            Predicate::new(
                move |state: &AccessState| state.peers.iter().any(|(_, d)| p.satisfied(d)),
                Some(&msg),
            )
        }

        pub(crate) fn with_identifier(id: PeerId) -> Predicate<Peer> {
            Predicate::<Peer>::new(
                move |d| d.identifier == id,
                Some(&format!("identifier == {}", id)),
            )
        }

        pub(crate) fn with_address(address: Address) -> Predicate<Peer> {
            Predicate::<Peer>::new(
                move |d| d.address == address,
                Some(&format!("address == {}", address)),
            )
        }

        pub(crate) fn connected(connected: bool) -> Predicate<Peer> {
            Predicate::<Peer>::new(
                move |d| d.connected == connected,
                Some(&format!("connected == {}", connected)),
            )
        }
    }

    pub fn peer_connected(id: &str, connected: bool) -> Predicate<AccessState> {
        peer::exists(peer::with_identifier(id).and(peer::connected(connected)))
    }

    pub fn peer_with_address(address: &str) -> Predicate<AccessState> {
        peer::exists(peer::with_address(address))
    }
}
