use {
    async_helpers::hanging_get::server as hanging_get,
    fidl_fuchsia_bluetooth as btfidl,
    fidl_fuchsia_bluetooth_sys as sys,
    fuchsia_bluetooth::{
        types::{Peer, PeerId},
    },
    fuchsia_syslog::fx_log_warn,
    std::collections::{
        HashMap, HashSet
    },
};

pub struct PeerWatcher {
    last_seen: HashMap<PeerId, Peer>,
    responder: sys::AccessWatchPeersResponder,
}

impl PeerWatcher {
    pub fn new(responder: sys::AccessWatchPeersResponder) -> PeerWatcher {
        PeerWatcher { last_seen: HashMap::new(), responder }
    }

    // Written as an associated function in order to match the signature of the HangingGet
    pub fn observe(new_peers: &HashMap<PeerId, Peer>, watcher: PeerWatcher) {
        let (updated, removed) = peers_diff(&watcher.last_seen, new_peers);
        let mut updated = updated.values().map(|p| p.into());
        let mut removed: Vec<btfidl::PeerId> = removed.iter().map(|&p| p.into()).collect();
        if let Err(err) = watcher.responder.send(&mut updated, &mut removed.iter_mut()) {
            fx_log_warn!("Unable to respond to watch_peers hanging get: {:?}", err);
        }
    }
}

fn peers_diff(prev: &HashMap<PeerId, Peer>, new: &HashMap<PeerId, Peer>) -> (HashMap<PeerId, Peer>, HashSet<PeerId>) {
    // Removed - those items in the prev set but not the new
    let removed: HashSet<PeerId> = prev.keys().filter(|id| !new.contains_key(id)).cloned().collect();
    // Updated - those items which are not present in same configuration in the prev set
    let updated = new.into_iter().filter(|(id, p)| !(prev.get(id) == Some(p))).map(|(id,p)| (*id, p.clone())).collect();
    (updated, removed)
}
