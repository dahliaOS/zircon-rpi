// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_bluetooth_control::ControlRequest;
use fidl_fuchsia_bluetooth_host::HostEvent;
use fuchsia_syslog::fx_log_warn;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use uuid::Uuid;

use crate::actor::{Actor, ActorHandle, ActorContext};
use crate::adapters::AdapterEvent::*;
use crate::host::{Host};
use crate::types::bt;
use crate::types::*;
use crate::types::DiscoveryState::*;
use crate::discovery::*;

/// --- THE NEW BT-GAP ---

/// The HostDispatcher acts as a proxy aggregating multiple Hosts
/// It appears as a Host to higher level systems, and is responsible for
/// routing commands to the appropriate Host

pub struct HostDispatcher<H: Host> {
    /// Bluetooth Hosts under management
    hosts: HashMap<HostId, H>,

    /// Currently 'active' host
    active_host: Option<HostId>,

    /// Discovery
    discovery:          DiscoveryState,
    discovery_sessions: HashSet<Uuid>,

    // TODO - how do we respond to ControlSession?
    //waiting_for_discovery: Vec<Oneshot::Sender<Result<()>>>
}

impl<H: Host> HostDispatcher<H> {
    pub fn new() -> HostDispatcher<H> {
        HostDispatcher {
            hosts: HashMap::new(),
            active_host: None,
            discovery: NotDiscovering,
            discovery_sessions: HashSet::new(),
        }
    }

    fn add_host(&mut self, id: HostId, host: H) {
        self.hosts.insert(id, host);
        // TODO - update states, e.g. discovery?
    }

    fn rm_host(&mut self, to_be_removed: &Path) {
        // only keep ones != our path, i.e. remove if it has our path
        self.hosts.retain(|_,h| h.path() != to_be_removed);
        // TODO - update states, e.g. discovery?
    }

    // TODO
    // We could just wait for a DiscoveryStarted message
    // What does the control fidl service method looks like?
    // They need to wait for a response - perhaps register a listener?
    pub fn start_discovery(&mut self, handle: ActorHandle<HostDispatcherMsg<H>>) -> bt::Result<DiscoverySession> {
        if !self.hosts.is_empty() {
            let session = DiscoverySession::new( handle.contramap(|CloseDiscoverySession{ session_id }| { HostDispatcherMsg::StopDiscovery(session_id) } ));
            self.discovery_sessions.insert( session.session_id );
            if self.discovery == NotDiscovering {
                self.enable_discovery()
            }
            Ok(session)
        } else {
            Err(bt::Error::NoAdapter)
        }
    }

    fn enable_discovery(&mut self) {
        // TODO - this should probably fail if *all* hosts fail to start_discovery
        // as long as one succeeds, report success
        // TODO - use the result
        for (_,host) in self.hosts.iter_mut() { let _r = host.start_discovery(); }
        self.discovery = Discovering;
    }

    fn disable_discovery(&mut self) {
        // TODO - use the result
        for (_,host) in self.hosts.iter_mut() { let _r = host.stop_discovery(); }
        self.discovery = NotDiscovering;
    }

    fn stop_discovery(&mut self, session_id: Uuid) {
        let existed = self.discovery_sessions.remove(&session_id);
        if !existed {
            fx_log_warn!("Tried to close a non-existent discovery session")
        }
        if self.discovery == Discovering && self.discovery_sessions.is_empty() {
            self.disable_discovery()
        }
    }

    fn activate_host(&mut self, id: HostId) -> Result<(), ()> {
        if self.active_host.as_ref() == Some(&id) {
            // It's already active; no-op
            return Ok(())
        }
        if let Some(_host) = self.hosts.get(&id) {
            // Activate `id`
            self.active_host = Some(id);
            // TODO - enable activated state
            //
            // // Initialize bt-gap as this host's pairing delegate.
            // start_pairing_delegate(self.clone(), host.clone())?;
            // // TODO(NET-1445): Only the active host should be made connectable and scanning in the background.
            // await!(host.set_connectable(true)).map_err(|_| err_msg("failed to set connectable"))?;
            // host.enable_background_scan(true).map_err(|_| err_msg("failed to enable background scan"))?;
            Ok(())
        } else {
            fx_log_warn!("Cannot activate host {}, it does not exist", id);
            Err(())
        }
    }
}

/*
impl HostDispatcher<FidlHost> {
    /// Adds an adapter to the host dispatcher. Called by the watch_hosts device watcher
    pub async fn add_adapter(&mut self, host: FidlHost) -> Result<(), Error> {
        fx_log_info!("Adding Host Device: {:?}", host.path());
        let info = host.get_info();
        // TODO
        //await!(try_restore_bonds(host.clone(), self.clone(), &info.address.clone()))?;

        self.add_host(info.identifier.into(), host);

        // Start listening to Host interface events.
        //fasync::spawn(host::run(self.clone(), host.clone()).map(|_| ()));

        Ok(())
    }
}
*/

pub enum HostDispatcherMsg<H> {
    StartDiscovery(ActorHandle<bt::Result<DiscoverySession>>),
    StopDiscovery(Uuid),
    AdapterAdded(HostId, H),
    AdapterRemoved(PathBuf),
    HostMsg(HostEvent),
    ControlMsg(ControlRequest),
}

impl<H: Host + Send + Sync> Actor for HostDispatcher<H> {
    type Message = HostDispatcherMsg<H>;

    fn update(&mut self, msg: Self::Message, _cx: ActorContext<HostDispatcher<H>>) {
        match msg {
            HostDispatcherMsg::StartDiscovery(mut sender) => {
                let session = self.start_discovery(_cx.handle);
                let _todo = sender.send(session);
            }
            HostDispatcherMsg::StopDiscovery(session_id) => {
                self.stop_discovery(session_id);
            }
            HostDispatcherMsg::HostMsg(_) => panic!("NYI"),
            HostDispatcherMsg::AdapterAdded(id, host) => self.add_host(id, host),
            HostDispatcherMsg::AdapterRemoved(path) => self.rm_host(&path),
            HostDispatcherMsg::ControlMsg(ControlRequest::RequestDiscovery{ .. }) => panic!("NYI"),
            HostDispatcherMsg::ControlMsg(_) => panic!("NYI"),
        }
    }
}

/*
#[cfg(test)]
mod test {

    #[test]
    fn test_discovery() {
        let dispatcher = HostDispatcher::new::<Fake::Host>();
        dispatcher.add_host(Fake::Host::new());
        let discovery = dispatcher.start_discovery();
        assert!(dispatcher.discovery == Discovering);
        Drop::drop(discovery);
        assert!(dispatcher.discovery == NotDiscovering);
    }

    /*
    #[test]
    fn test_multi_discovery() {
        let dispatcher = HostDispatcher::new();
        dispatcher.add_host(Host::fake());
        let discoveries = [1..3].map(|_| dispatcher.start_discovery());
        let (head, tail) = discoveries.randomize().split_at(1);
        for d in tail {
            Drop::drop(d);
            assert!(dispatcher.discovery == Discovering);
        }
        Drop::drop(head);
        assert!(dispatcher.discovery == NotDiscovering);
    }
    */
}
*/
















/*
impl HostDispatcher {
    // Adapter Management
    pub fn hosts(&self) -> Vec<AdapterInfo>;
    pub fn active_adapter(&self) -> Option<AdapterInfo>;
    pub fn set_active_adapter(&mut self, id: HostId) -> bt::Result<()>;

    // Device listing
    pub fn remote_devices(&self) -> Vec<RemoteDevice>;

    // Device Control
    pub async fn connect(&mut self, device_id: String) -> bt::Result<()>;
    pub async fn disconnect(&mut self, device_id: String) -> bt::Result<()>;
    pub async fn forget(&mut self, _device_id: String) -> bt::Result<()>;

    // Adapter Control
    pub async fn set_name(&mut self, name: Option<String>) -> bt::Result<()>;
    pub async fn start_discovery(&mut self) -> bt::Result<DiscoverySession>;
    pub async fn set_discoverable(&mut self) -> DiscoverableRequest;

    // System control
    pub fn subscribe_device_events(&mut self, handle: Weak<ControlControlHandle>); // TODO can we take a channel instead of fidl handle?
    // pub fn subscribe_device_events(&mut self, chan: Channel<RemoteDeviceEvent>);
    pub fn request_host_service(mut self, chan: fasync::Channel, service: HostService);
    pub fn set_pairing_delegate(&mut self, delegate: Option<PairingDelegateProxy>) -> bool;
    pub fn set_io_capability(&mut self, input: InputCapabilityType, output: OutputCapabilityType);

    // Bonding
    pub fn store_bond(&mut self, bond: BondingData) -> Result<(),Error>;
}
*/

/*
pub fn rm_adapter(&mut self, adapter: Adapter) {
    hosts -= adapter;

    if hosts.empty() {
        // terminate discovery
        disable_discovery();
        discovery_sessions.notify(DiscoveryTerminated);
    }
}

pub fn start_discoverable(&Mut self) -> DiscoverableRequest {
    DiscoverableRequest{ self }
}

/// Set the active adapter for this HostDispatcher
pub fn set_active_adapter(&mut self, id: HostId) -> Result<()> {
    if self.hosts.contains_key(id) {
        let current = self.active_adapter();
        if current != id {
            self.deactivate(current);
        }
        self.activate(id);
        Ok(())
    } else {
        Err(MissingAdapter)
    }
}

/// Used to set the pairing delegate. If there is a prior pairing delegate connected to the
/// host it will fail. It checks if the existing stored connection is closed, and will
/// overwrite it if so.
// TODO - is this the right return type? Perhaps Result instead?
// Should we be returning false if we successfully cleared the delegate?
pub fn set_pairing_delegate(&mut self, delegate: Option<PairingDelegateProxy>) -> bool {
    if self.pairing_delegate.exists(|d| !d.is_closed()) {
        false
    } else {
        self.pairing_delegate = delegate;
        delegate.is_some()
    }
}

/// Add a new host to the dispatcher
fn add_host(&mut self, id: HostId, host: Adapter) {
    fx_log_info!("Host added: {:?}", host.read().get_info().identifier);
    let info = host.get_info();
    self.hosts.insert(id, host);

    // Notify Control interface clients about the new device.
    self.notify_event_listeners(|l| {
        let _res = l.send_on_adapter_updated(&mut clone_host_info(&info));
    });
}

/// Remove a host from the dispatcher
fn rm_host(&mut self, id: HostId, host: Host) {
    fx_log_info!("Host removed: {:?}", host.get_info().identifier);
}
*/
