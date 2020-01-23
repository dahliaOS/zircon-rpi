// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::format_err,
    bt_a2dp::media_types::*,
    bt_a2dp_sink_metrics as metrics, bt_avdtp as avdtp,
    fidl::encoding::Decodable,
    fidl_fuchsia_bluetooth_bredr::{ChannelParameters, ProfileDescriptor, ProfileProxy, PSM_AVDTP},
    fuchsia_async::{self as fasync, DurationExt},
    fuchsia_bluetooth::{
        detachable_map::{DetachableMap, DetachableWeak},
        types::PeerId,
    },
    fuchsia_cobalt::CobaltSender,
    fuchsia_inspect as inspect,
    fuchsia_syslog::{fx_log_info, fx_log_warn, fx_vlog},
    fuchsia_zircon as zx,
    parking_lot::{Mutex, RwLock},
    std::{
        collections::hash_map::Entry,
        collections::{HashMap, HashSet},
        convert::TryInto,
        sync::Arc,
    },
};

use crate::{avrcp_relay::AvrcpRelay, peer, Streams};

// Duration for A2DP-SNK to wait before assuming role of the initiator.
// If an L2CAP signaling channel has not been established by this time, A2DP-Sink will
// create the signaling channel, configure, open and start the stream.
const A2DP_SNK_AS_INT_THRESHOLD: zx::Duration = zx::Duration::from_seconds(1);

// Arbitrarily chosen ID for the SBC stream endpoint.
pub(crate) const SBC_SEID: u8 = 6;

fn codectype_to_availability_metric(
    codec_type: avdtp::MediaCodecType,
) -> metrics::A2dpCodecAvailabilityMetricDimensionCodec {
    match codec_type {
        avdtp::MediaCodecType::AUDIO_SBC => metrics::A2dpCodecAvailabilityMetricDimensionCodec::Sbc,
        avdtp::MediaCodecType::AUDIO_MPEG12 => {
            metrics::A2dpCodecAvailabilityMetricDimensionCodec::Mpeg12
        }
        avdtp::MediaCodecType::AUDIO_AAC => metrics::A2dpCodecAvailabilityMetricDimensionCodec::Aac,
        avdtp::MediaCodecType::AUDIO_ATRAC => {
            metrics::A2dpCodecAvailabilityMetricDimensionCodec::Atrac
        }
        avdtp::MediaCodecType::AUDIO_NON_A2DP => {
            metrics::A2dpCodecAvailabilityMetricDimensionCodec::VendorSpecific
        }
        _ => metrics::A2dpCodecAvailabilityMetricDimensionCodec::Unknown,
    }
}

fn spawn_stream_discovery(peer: &peer::Peer) {
    let collect_fut = peer.collect_capabilities();
    let remote_capabilities_inspect = peer.remote_capabilities_inspect();
    let mut cobalt = peer.cobalt_logger();

    let discover_fut = async move {
        // Store deduplicated set of codec event codes for logging.
        let mut codec_event_codes = HashSet::new();

        let streams = match collect_fut.await {
            Ok(streams) => streams,
            Err(e) => {
                fx_log_info!("Collecting capabilities failed: {:?}", e);
                return;
            }
        };

        for stream in streams {
            let capabilities = stream.capabilities();
            remote_capabilities_inspect.append(stream.local_id(), &capabilities).await;
            for cap in capabilities {
                if let avdtp::ServiceCapability::MediaCodec {
                    media_type: avdtp::MediaType::Audio,
                    codec_type,
                    ..
                } = cap
                {
                    codec_event_codes
                        .insert(codectype_to_availability_metric(codec_type.clone()) as u32);
                }
            }
        }

        for event_code in codec_event_codes {
            cobalt.log_event(metrics::A2DP_CODEC_AVAILABILITY_METRIC_ID, event_code);
        }
    };
    fasync::spawn(discover_fut);
}

pub struct ConnectedPeers {
    inner: Arc<Mutex<ConnectedPeersInner>>,
}

impl ConnectedPeers {
    pub(crate) fn new(
        streams: Streams,
        profile: ProfileProxy,
        cobalt_sender: CobaltSender,
        domain: Option<String>,
    ) -> Self {
        Self {
            inner: Arc::new(Mutex::new(ConnectedPeersInner::new(
                streams,
                profile,
                cobalt_sender,
                domain,
            ))),
        }
    }

    pub(crate) fn get(&self, id: &PeerId) -> Option<Arc<RwLock<peer::Peer>>> {
        self.inner.lock().get(id)
    }

    pub(crate) fn profile(&self) -> ProfileProxy {
        self.inner.lock().profile.clone()
    }

    pub fn found(&mut self, inspect: &inspect::Inspector, id: PeerId, desc: ProfileDescriptor) {
        if self.inner.lock().descriptors.insert(id, Some(desc)).is_some() {
            // We have maybe connected to this peer before, and we just need to
            // discover the streams.
            if let Some(peer) = self.get(&id) {
                peer.write().set_descriptor(desc.clone());
                spawn_stream_discovery(&peer.read());
            }
        } else {
            // If the peer has not connected to us, then sink may potentially need to play the
            // INT role.
            //
            // Wait A2DP_SNK_AS_INT_THRESHOLD time, and see if `peer_id` connects.
            //
            // If not, sink will configure the remote stream endpoint, open the media transport
            // connection, and call start stream if the connection is idle.
            fx_vlog!(
                tag: "a2dp-sink",
                1,
                "A2DP sink - waiting {:?} seconds before assuming INT role for peer {}.",
                A2DP_SNK_AS_INT_THRESHOLD,
                id,
            );

            let timer_expired = fuchsia_async::Timer::new(A2DP_SNK_AS_INT_THRESHOLD.after_now());
            let inner_clone = self.inner.clone();
            let inspect_clone = inspect.clone();
            let profile = self.profile();

            fasync::spawn(async move {
                timer_expired.await;

                if inner_clone.lock().contains_peer(&id) {
                    fx_vlog!(
                        tag: "a2dp-sink",
                        1,
                        "Peer {} has already connected. A2DP sink will not assume the INT role.",
                        id
                    );
                    return;
                }

                fx_vlog!(tag: "a2dp-sink", 1, "Remote peer has not established connection. A2DP sink will now assume the INT role.");
                let (status, channel) = profile
                    .connect_l2cap(
                        &id.to_string(),
                        PSM_AVDTP as u16,
                        ChannelParameters::new_empty(),
                    )
                    .await
                    .unwrap();
                if let Some(e) = status.error {
                    fx_log_warn!("Couldn't connect media transport {}: {:?}", id, e);
                    return;
                }
                if channel.socket.is_none() {
                    fx_log_warn!("Couldn't connect media transport {}: no channel", id);
                    return;
                }

                {
                    let mut inner = inner_clone.lock();
                    inner.create_and_start_peer(
                        &inspect_clone,
                        id,
                        channel.socket.expect("Just checked contents"),
                        true, // Start the streaming task because A2DP-sink has assumed the INT role.
                    );
                }
            });
        }
    }

    pub fn connected(&mut self, inspect: &inspect::Inspector, id: PeerId, channel: zx::Socket) {
        match self.get(&id) {
            Some(peer) => {
                if let Err(e) = peer.write().receive_channel(channel) {
                    fx_log_warn!("{} failed to connect channel: {}", id, e);
                }
            }
            None => {
                fx_log_info!("Adding new peer for {}", id);
                {
                    let mut inner = self.inner.lock();
                    // Store the signaling channel associated with the peer.
                    inner.create_and_start_peer(
                        inspect, id, channel,
                        false, // Peer connected to us. Don't initiate streaming.
                    );
                }
            }
        }
    }
}

/// ConnectedPeersInner owns the set of connected peers and manages peers based on
/// discovery, connections and disconnections.
pub struct ConnectedPeersInner {
    /// The set of connected peers.
    connected: DetachableMap<PeerId, RwLock<peer::Peer>>,
    /// The proxy used to connect transport sockets.
    profile: ProfileProxy,
    /// ProfileDescriptors from discovering the peer.
    descriptors: HashMap<PeerId, Option<ProfileDescriptor>>,
    /// The set of streams that are made available to peers.
    streams: Streams,
    /// Cobalt logger to use and hand out to peers
    cobalt_sender: CobaltSender,
    /// Media session domain
    domain: Option<String>,
}

impl ConnectedPeersInner {
    pub(crate) fn new(
        streams: Streams,
        profile: ProfileProxy,
        cobalt_sender: CobaltSender,
        domain: Option<String>,
    ) -> Self {
        Self {
            connected: DetachableMap::new(),
            profile,
            descriptors: HashMap::new(),
            streams,
            cobalt_sender,
            domain,
        }
    }

    pub(crate) fn get(&self, id: &PeerId) -> Option<Arc<RwLock<peer::Peer>>> {
        self.connected.get(id).and_then(|p| p.upgrade())
    }

    pub(crate) fn contains_peer(&self, id: &PeerId) -> bool {
        self.connected.contains_key(id)
    }

    async fn start_streaming(
        peer: &DetachableWeak<PeerId, RwLock<peer::Peer>>,
    ) -> Result<(), anyhow::Error> {
        let strong = peer.upgrade().ok_or(format_err!("Disconnected"))?;
        let remote_streams = strong.read().collect_capabilities().await?;

        // Find the SBC stream, which should exist (it is required)
        let remote_stream = remote_streams
            .iter()
            .filter(|stream| stream.information().endpoint_type() == &avdtp::EndpointType::Source)
            .find(|stream| stream.codec_type() == Some(&avdtp::MediaCodecType::AUDIO_SBC))
            .ok_or(format_err!("Couldn't find a compatible stream"))?;

        // TODO(39321): Choose codec options based on availability and quality.
        let sbc_media_codec_info = SbcCodecInfo::new(
            SbcSamplingFrequency::FREQ44100HZ,
            SbcChannelMode::JOINT_STEREO,
            SbcBlockCount::SIXTEEN,
            SbcSubBands::EIGHT,
            SbcAllocation::LOUDNESS,
            SbcCodecInfo::BITPOOL_MIN,
            53,
        )?;

        let sbc_settings = avdtp::ServiceCapability::MediaCodec {
            media_type: avdtp::MediaType::Audio,
            codec_type: avdtp::MediaCodecType::AUDIO_SBC,
            codec_extra: sbc_media_codec_info.to_bytes(),
        };

        let strong = peer.upgrade().ok_or(format_err!("Disconnected"))?;
        let _ = strong
            .read()
            .start_stream(
                SBC_SEID.try_into().unwrap(),
                remote_stream.local_id().clone(),
                sbc_settings.clone(),
            )
            .await?;
        Ok(())
    }

    /// Set up the avdtp_peer given a signaling `channel`.
    /// Starts the AVRCP relay.
    ///
    /// If `initiate_streaming` = true, this method will spawn the streaming task.
    fn create_and_start_peer(
        &mut self,
        inspect: &inspect::Inspector,
        id: PeerId,
        channel: zx::Socket,
        initiate_streaming: bool,
    ) {
        let avdtp_peer = match avdtp::Peer::new(channel) {
            Ok(peer) => peer,
            Err(e) => {
                fx_log_warn!("Error adding signaling peer {}: {:?}", id, e);
                return;
            }
        };
        let inspect = inspect.root().create_child(format!("peer {}", id));
        let mut peer = peer::Peer::create(
            id,
            avdtp_peer,
            self.streams.clone(),
            self.profile.clone(),
            inspect,
            self.cobalt_sender.clone(),
        );

        // Start remote discovery if profile information exists for the device_id
        // and a2dp sink not assuming the INT role.
        match self.descriptors.entry(id) {
            Entry::Occupied(entry) => {
                if let Some(prof) = entry.get() {
                    peer.set_descriptor(prof.clone());
                    if !initiate_streaming {
                        spawn_stream_discovery(&peer);
                    }
                }
            }
            // Otherwise just insert the device ID with no profile
            // Run discovery when profile is updated
            Entry::Vacant(entry) => {
                entry.insert(None);
            }
        }

        let closed_fut = peer.closed();
        self.connected.insert(id, RwLock::new(peer));

        let avrcp_relay = AvrcpRelay::start(id, self.domain.clone()).ok();

        if initiate_streaming {
            let weak_peer = self.connected.get(&id).expect("just added");
            fuchsia_async::spawn_local(async move {
                if let Err(e) = ConnectedPeersInner::start_streaming(&weak_peer).await {
                    fx_vlog!(tag: "a2dp-sink", 1, "Streaming task ended: {:?}", e);
                    weak_peer.detach();
                }
            });
        }

        // Remove the peer when we disconnect.
        let detached_peer = self.connected.get(&id).expect("just added");
        let mut descriptors = self.descriptors.clone();
        let disconnected_id = id.clone();
        fasync::spawn(async move {
            closed_fut.await;
            detached_peer.detach();
            descriptors.remove(&disconnected_id);
            // Captures the relay to extend the lifetime until after the peer closes.
            drop(avrcp_relay);
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use bt_avdtp::Request;
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_bluetooth::Status;
    use fidl_fuchsia_bluetooth_bredr::{
        Channel, ProfileMarker, ProfileRequest, ProfileRequestStream, ServiceClassProfileIdentifier,
    };
    use fidl_fuchsia_cobalt::CobaltEvent;
    use futures::channel::mpsc;
    use futures::{self, task::Poll, StreamExt};
    use std::convert::TryFrom;

    fn fake_cobalt_sender() -> (CobaltSender, mpsc::Receiver<CobaltEvent>) {
        const BUFFER_SIZE: usize = 100;
        let (sender, receiver) = mpsc::channel(BUFFER_SIZE);
        (CobaltSender::new(sender), receiver)
    }

    fn run_to_stalled(exec: &mut fasync::Executor) {
        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());
    }

    fn exercise_avdtp(exec: &mut fasync::Executor, remote: zx::Socket, peer: &peer::Peer) {
        let remote_avdtp = avdtp::Peer::new(remote).expect("remote control should be creatable");
        let mut remote_requests = remote_avdtp.take_request_stream();

        // Should be able to actually communicate via the peer.
        let avdtp = peer.avdtp_peer();
        let discover_fut = avdtp.discover();

        futures::pin_mut!(discover_fut);

        assert!(exec.run_until_stalled(&mut discover_fut).is_pending());

        let responder = match exec.run_until_stalled(&mut remote_requests.next()) {
            Poll::Ready(Some(Ok(Request::Discover { responder }))) => responder,
            x => panic!("Expected a Ready Discovery request but got {:?}", x),
        };

        let endpoint_id = avdtp::StreamEndpointId::try_from(1).expect("endpointid creation");

        let information = avdtp::StreamInformation::new(
            endpoint_id,
            false,
            avdtp::MediaType::Audio,
            avdtp::EndpointType::Source,
        );

        responder.send(&[information]).expect("Sending response should have worked");

        let _stream_infos = match exec.run_until_stalled(&mut discover_fut) {
            Poll::Ready(Ok(infos)) => infos,
            x => panic!("Expected a Ready response but got {:?}", x),
        };
    }

    fn setup_connected_peer_test(
    ) -> (fasync::Executor, PeerId, ConnectedPeers, inspect::Inspector, ProfileRequestStream) {
        let exec = fasync::Executor::new_with_fake_time().expect("executor should build");
        let (proxy, stream) =
            create_proxy_and_stream::<ProfileMarker>().expect("Profile proxy should be created");
        let id = PeerId(1);
        let (cobalt_sender, _) = fake_cobalt_sender();

        let peers = ConnectedPeers::new(Streams::new(), proxy, cobalt_sender, None);

        let inspect = inspect::Inspector::new();

        (exec, id, peers, inspect, stream)
    }

    #[test]
    fn connected_peers_connect_creates_peer() {
        let (mut exec, id, mut peers, inspect, _stream) = setup_connected_peer_test();

        let (remote, signaling) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();

        peers.connected(&inspect, id, signaling);

        let peer = match peers.get(&id) {
            None => panic!("Peer should be in ConnectedPeers after connection"),
            Some(peer) => peer,
        };

        exercise_avdtp(&mut exec, remote, &peer.read());
    }

    fn expect_started_discovery(exec: &mut fasync::Executor, remote: zx::Socket) {
        let remote_avdtp = avdtp::Peer::new(remote).expect("remote control should be creatable");
        let mut remote_requests = remote_avdtp.take_request_stream();

        // Start of discovery is by discovering the peer streaminformations.
        let _ = match exec.run_until_stalled(&mut remote_requests.next()) {
            Poll::Ready(Some(Ok(Request::Discover { responder }))) => responder,
            x => panic!("Expected to get a discovery request but got {:?}", x),
        };
    }

    #[test]
    fn connected_peers_found_connected_peer_starts_discovery() {
        let (mut exec, id, mut peers, inspect, _stream) = setup_connected_peer_test();

        let (remote, signaling) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();

        peers.connected(&inspect, id, signaling);

        let profile_desc = ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 3,
        };

        peers.found(&inspect, id, profile_desc);

        expect_started_discovery(&mut exec, remote);
    }

    #[test]
    fn connected_peers_connected_found_peer_starts_discovery() {
        let (mut exec, id, mut peers, inspect, _stream) = setup_connected_peer_test();

        let (remote, signaling) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();

        let profile_desc = ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 3,
        };

        peers.found(&inspect, id, profile_desc);

        peers.connected(&inspect, id, signaling);

        expect_started_discovery(&mut exec, remote);
    }

    #[test]
    fn connected_peers_peer_disconnect_removes_peer() {
        let (mut exec, id, mut peers, inspect, _stream) = setup_connected_peer_test();

        let (remote, signaling) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();

        peers.connected(&inspect, id, signaling);
        run_to_stalled(&mut exec);

        // Disconnect the signaling channel, peer should be gone.
        drop(remote);

        run_to_stalled(&mut exec);

        assert!(peers.get(&id).is_none());
    }

    #[test]
    fn connected_peers_reconnect_works() {
        let (mut exec, id, mut peers, inspect, _stream) = setup_connected_peer_test();

        let (remote, signaling) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();

        peers.connected(&inspect, id, signaling);
        run_to_stalled(&mut exec);

        // Disconnect the signaling channel, peer should be gone.
        drop(remote);

        run_to_stalled(&mut exec);

        assert!(peers.get(&id).is_none());

        // Connect another peer with the same ID
        let (_remote, signaling) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();

        peers.connected(&inspect, id, signaling);
        run_to_stalled(&mut exec);

        // Should be connected.
        assert!(peers.get(&id).is_some());
    }

    #[test]
    /// Tests that A2DP sink assumes the initiator role when a peer is found, but
    /// not connected, and the timeout completes.
    fn wait_to_initiate_success_with_no_connected_peer() {
        let (mut exec, id, mut peers, inspect, mut profile_request_stream) =
            setup_connected_peer_test();
        // Initialize context to a fixed point in time.
        exec.set_fake_time(fasync::Time::from_nanos(1000000000));

        let profile_desc = ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 3,
        };

        // A remote peer was found, but hasn't connected yet. There should be no entry for it.
        peers.found(&inspect, id, profile_desc);
        run_to_stalled(&mut exec);
        assert!(peers.get(&id).is_none());

        // Fast forward time by 5 seconds.
        exec.set_fake_time(fasync::Time::from_nanos(6000000000));
        exec.wake_expired_timers();
        run_to_stalled(&mut exec);

        // Should connect the media socket after open.
        let (_test, transport) =
            zx::Socket::create(zx::SocketOpts::DATAGRAM).expect("socket creation fail");
        let request = exec.run_until_stalled(&mut profile_request_stream.next());
        match request {
            Poll::Ready(Some(Ok(ProfileRequest::ConnectL2cap { peer_id, responder, .. }))) => {
                assert_eq!(PeerId(1), peer_id.parse().expect("peer_id parses"));
                responder
                    .send(
                        &mut Status { error: None },
                        Channel { socket: Some(transport), ..Channel::new_empty() },
                    )
                    .expect("responder sends");
            }
            x => panic!("Should have sent a open l2cap request, but got {:?}", x),
        };

        run_to_stalled(&mut exec);

        // Even though the remote peer did not connect, A2DP Sink should initiate a connection
        // and insert into `peers`.
        assert!(peers.get(&id).is_some());
    }

    #[test]
    /// Tests that A2DP sink does not assume the initiator role when a peer connects
    /// before `A2DP_SNK_AS_INT_THRESHOLD` timeout completes.
    fn wait_to_initiate_returns_early_with_connected_peer() {
        let (mut exec, id, mut peers, inspect, _profile_request_stream) =
            setup_connected_peer_test();
        let (remote, signaling) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();

        // Initialize context to a fixed point in time.
        exec.set_fake_time(fasync::Time::from_nanos(1000000000));

        let profile_desc = ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 3,
        };

        // A remote peer was found, but hasn't connected yet. There should be no entry for it.
        peers.found(&inspect, id, profile_desc);
        run_to_stalled(&mut exec);
        assert!(peers.get(&id).is_none());

        // Fast forward time by .5 seconds.
        exec.set_fake_time(fasync::Time::from_nanos(1500000000));
        exec.wake_expired_timers();
        run_to_stalled(&mut exec);

        // A peer connects before the timeout.
        peers.connected(&inspect, id, signaling);

        // Discovery should occur as spawned by `connected`.
        expect_started_discovery(&mut exec, remote);
    }
}
