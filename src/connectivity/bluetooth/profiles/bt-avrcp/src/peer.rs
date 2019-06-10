// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    bt_avctp::{
        AvcCommand, AvcCommandResponse, AvcCommandStream, AvcCommandType, AvcPacketType, AvcPeer,
        AvcResponseType, Error as AvctpError,
    },
    failure::{format_err, Error as FailureError},
    fidl::encoding::Decodable as FidlDecodable,
    fidl_fuchsia_bluetooth_avrcp::{AvcPanelCommand, MediaAttributes},
    fidl_fuchsia_bluetooth_bredr::PSM_AVCTP,
    fuchsia_async as fasync,
    fuchsia_syslog::{self, fx_log_err, fx_log_info, fx_vlog},
    fuchsia_zircon as zx,
    futures::{
        self, channel::mpsc, channel::oneshot, future::FutureExt, future::FutureObj,
        future::LocalFutureObj, ready, stream::FusedStream, stream::FuturesUnordered, stream::Map,
        stream::SelectAll, stream::StreamExt, task::Context, Poll, Stream,
    },
    parking_lot::{Mutex, RwLock, RwLockUpgradableReadGuard},
    std::{
        collections::HashMap, convert::TryFrom, pin::Pin, string::String, sync::Arc, sync::Weak,
    },
};

use crate::{
    packets::{Error as PacketError, *},
    profile::{AvrcpProfile, AvrcpProfileEvent, AvrcpService},
    types::{PeerError as Error, PeerId},
};

#[derive(Debug, PartialEq)]
enum PeerChannel<T> {
    Connected(Arc<T>),
    Connecting,
    Disconnected,
}

impl<T> PeerChannel<T> {
    fn connection(&self) -> Option<Arc<T>> {
        match self {
            PeerChannel::Connected(t) => Some(t.clone()),
            _ => None,
        }
    }
}

/// Internal object to manage a remote peer
#[derive(Debug)]
struct RemotePeer {
    peer_id: PeerId,

    /// Contains the remote peer's target profile.
    target_descriptor: RwLock<Option<AvrcpService>>,

    /// Contains the remote peer's controller profile.
    controller_descriptor: RwLock<Option<AvrcpService>>,

    /// Control channel to the remote device.
    control_channel: RwLock<PeerChannel<AvcPeer>>,

    // Todo: add browse channel.
    // browse_channel: RwLock<PeerChannel<AvtcpPeer>>,
    //
    /// Contains a vec of all PeerControllers that have taken an event stream waiting for events from this peer.
    controller_listeners: Mutex<Vec<mpsc::Sender<PeerControllerEvent>>>,

    /// Process peer commands and holds state for the control channel
    command_handler: Mutex<Option<ControlChannelCommandHandler>>,
}

impl RemotePeer {
    fn new(peer_id: PeerId) -> Self {
        Self {
            peer_id,
            control_channel: RwLock::new(PeerChannel::Disconnected),
            //browse_channel: RwLock::new(PeerChannel::Disconnected),
            controller_listeners: Mutex::new(Vec::new()),
            target_descriptor: RwLock::new(None),
            controller_descriptor: RwLock::new(None),
            command_handler: Mutex::new(None),
        }
    }

    /// Enumerates all listening controller_listeners queues and sends a clone of the event to each
    fn broadcast_event(&self, event: PeerControllerEvent) {
        let mut listeners = self.controller_listeners.lock();
        // remove all the dead listeners from the list.
        listeners.retain(|i| !i.is_closed());
        for sender in listeners.iter_mut() {
            if let Err(send_error) = sender.try_send(event.clone()) {
                fx_log_err!(
                    "unable to send event to peer controller stream for {} {:?}",
                    self.peer_id,
                    send_error
                );
            }
        }
    }

    // Hold the write lock on control_channel before calling this.
    fn reset_command_handler(&self) {
        let mut cmd_handler = self.command_handler.lock();
        *cmd_handler = None;
    }

    fn reset_connection(&self) {
        let mut control_channel = self.control_channel.write();
        self.reset_command_handler();
        *control_channel = PeerChannel::Disconnected;
    }
}

#[derive(Debug)]
pub struct PeerControllerRequest {
    // Peer we wants
    peer_id: PeerId,
    // One shot channel to reply to this request.
    reply: oneshot::Sender<PeerController>,
}

impl PeerControllerRequest {
    pub fn new(peer_id: PeerId) -> (oneshot::Receiver<PeerController>, PeerControllerRequest) {
        let (sender, receiver) = oneshot::channel();
        (receiver, Self { peer_id: peer_id.clone(), reply: sender })
    }
}

type RemotePeersMap = HashMap<PeerId, Arc<RemotePeer>>;

pub struct PeerManager {
    inner: Arc<PeerManagerInner>,
    peer_request: mpsc::Receiver<PeerControllerRequest>,
    new_control_connection_futures:
        FuturesUnordered<FutureObj<'static, Result<(PeerId, zx::Socket), Error>>>,
    event_pump_futures: FuturesUnordered<LocalFutureObj<'static, ()>>,
    control_channel_streams: SelectAll<
        Map<
            AvcCommandStream,
            Box<
                FnMut(Result<AvcCommand, AvctpError>) -> (String, Result<AvcCommand, AvctpError>)
                    + Send,
            >,
        >,
    >,
}

impl PeerManager {
    pub fn new(
        profile_svc: AvrcpProfile,
        peer_request: mpsc::Receiver<PeerControllerRequest>,
    ) -> Result<PeerManager, FailureError> {
        Ok(Self {
            inner: Arc::new(PeerManagerInner::new(profile_svc)),
            peer_request,
            new_control_connection_futures: FuturesUnordered::new(),
            event_pump_futures: FuturesUnordered::new(),
            control_channel_streams: SelectAll::new(),
        })
    }

    fn connect_remote_control_psm(&mut self, peer_id: &str, psm: u16) {
        let remote_peer = self.inner.get_remote_peer(peer_id);

        let connection = remote_peer.control_channel.upgradable_read();
        match *connection {
            PeerChannel::Disconnected => {
                let mut conn = RwLockUpgradableReadGuard::upgrade(connection);
                *conn = PeerChannel::Connecting;
                let inner = self.inner.clone();
                let peer_id = String::from(peer_id);
                self.new_control_connection_futures.push(FutureObj::new(
                    async move {
                        let socket = await!(inner.profile_svc.connect_to_device(&peer_id, psm))
                            .map_err(|e| {
                                Error::ConnectionFailure(format_err!("Connection error: {:?}", e))
                            })?;
                        Ok((peer_id, socket))
                    }
                        .boxed(),
                ));
            }
            _ => return,
        }
    }

    fn new_control_connection(&self, peer_id: &PeerId, channel: zx::Socket) {
        let remote_peer = self.inner.get_remote_peer(peer_id);
        match AvcPeer::new(channel) {
            Ok(peer) => {
                fx_vlog!(tag: "avrcp", 1, "new peer {:#?}", peer);
                let mut connection = remote_peer.control_channel.write();
                remote_peer.reset_command_handler();
                *connection = PeerChannel::Connected(Arc::new(peer));
            }
            Err(e) => {
                fx_log_err!("Unable to make peer from socket {}: {:?}", peer_id, e);
            }
        }
    }

    fn connect_to_peer_if_necessary(&mut self, peer_id: &PeerId) {
        fx_vlog!(tag: "avrcp", 2, "connect_to_peer_if_necessary {:#?}", peer_id);
        let remote_peer = self.inner.get_remote_peer(peer_id);

        // we are already processing this stream.
        let mut command_handle_guard = remote_peer.command_handler.lock();

        if command_handle_guard.is_some() {
            fx_vlog!(tag: "avrcp", 2, "connect_to_peer_if_necessary, already processing stream {:#?}", peer_id);
            return;
        }

        if remote_peer.target_descriptor.read().is_none()
            && remote_peer.controller_descriptor.read().is_none()
        {
            // we don't have the profile information on this steam.
            fx_vlog!(tag: "avrcp", 2, "connect_to_peer_if_necessary, no profile descriptor yet {:#?}", peer_id);
            return;
        }

        let connection = remote_peer.control_channel.read();
        match connection.connection() {
            Some(peer_connection) => {
                fx_vlog!(tag: "avrcp", 2, "connect_to_peer_if_necessary, no profile descriptor yet {:#?}", peer_id);
                // we have a connection with a profile descriptor but we aren't processing it yet.

                *command_handle_guard =
                    Some(ControlChannelCommandHandler::new(Arc::downgrade(&remote_peer)));

                let pid = peer_id.clone();

                // The rust compiler won't cast our boxed closure as a FnMut directly but will through
                // the return of a function.
                fn get_closure_as_fn(
                    pid: PeerId,
                ) -> Box<
                    FnMut(
                            Result<AvcCommand, AvctpError>,
                        ) -> (PeerId, Result<AvcCommand, AvctpError>)
                        + Send,
                > {
                    Box::new(move |command| (pid.clone(), command))
                }

                let stream = peer_connection.take_command_stream().map(get_closure_as_fn(pid));
                self.control_channel_streams.push(stream);
                self.pump_peer_notifications(peer_id);
            }
            None => {
                // drop our write guard
                drop(connection);
                // we have a profile descriptor but we aren't connected to it yet.
                // Todo: extract out the dynamic PSM from the profile.
                self.connect_remote_control_psm(peer_id, PSM_AVCTP as u16);
            }
        }
    }

    fn handle_profile_service_event(&mut self, event: AvrcpProfileEvent) {
        match event {
            AvrcpProfileEvent::IncomingConnection { peer_id, channel } => {
                fx_vlog!(tag: "avrcp", 2, "IncomingConnection {} {:#?}", peer_id, channel);
                self.new_control_connection(&peer_id, channel);
                self.connect_to_peer_if_necessary(&peer_id);
            }
            AvrcpProfileEvent::ServicesDiscovered { peer_id, services } => {
                fx_vlog!(tag: "avrcp", 2, "ServicesDiscovered {} {:#?}", peer_id, services);
                let remote_peer = self.inner.get_remote_peer(&peer_id);
                for service in services {
                    match service {
                        AvrcpService::Target { .. } => {
                            let mut profile_descriptor = remote_peer.target_descriptor.write();
                            *profile_descriptor = Some(service);
                        }
                        AvrcpService::Controller { .. } => {
                            let mut profile_descriptor = remote_peer.controller_descriptor.write();
                            *profile_descriptor = Some(service);
                        }
                    }
                }

                self.connect_to_peer_if_necessary(&peer_id);
            }
        }
    }

    fn pump_peer_notifications(&mut self, peer_id: &PeerId) {
        // events we support when speaking to a peer that supports the controller profile.
        //const REMOTE_CONTROLLER_SUPPORTED_NOTIFICATIONS: [NotificationEventId; 1] =
        //    [NotificationEventId::EventVolumeChanged];

        // events we support when speaking to a peer that supports the target profile.
        const REMOTE_TARGET_SUPPORTED_NOTIFICATIONS: [NotificationEventId; 3] = [
            NotificationEventId::EventPlaybackStatusChanged,
            NotificationEventId::EventTrackChanged,
            NotificationEventId::EventPlaybackPosChanged,
        ];

        // TODO: parse the peer to see if it behaves as target profile or a controller profile.
        //       For now register for everything.
        let supported_notifications: Vec<NotificationEventId> =
            REMOTE_TARGET_SUPPORTED_NOTIFICATIONS.iter().cloned().collect();

        // look up what notifications we support on this peer first
        /*let supported_notifications: Vec<NotificationEventId> =
            await!(self.inner.get_supported_events(peer_id))?
                .iter()
                .cloned()
                .filter(|k| local_supported_notifications.contains(k))
                .collect();
        */

        let shared_peer = self.inner.get_remote_peer(peer_id);
        let shared_peer_id = peer_id.clone();
        fasync::spawn_local(async move {
            let mut notifications = FuturesUnordered::new();

            for notif in supported_notifications {
                let peer = shared_peer.clone();
                let peer_id = shared_peer_id.clone();

                // returns true if error is unrecoverable and we should stop processing events on the stream
                // Todo: possibly close? look for races with shutdown
                let fut = async move {
                    fx_vlog!(tag: "avrcp", 2, "creating notification stream for {:#?}", notif);
                    let mut stream = NotificationStream::new(peer.clone(), &peer_id, notif, 5);
                    fn handle_response(
                        notif: &NotificationEventId,
                        peer: &Arc<RemotePeer>,
                        data: &[u8],
                    ) -> Result<bool, Error> {
                        fx_vlog!(tag: "avrcp", 2, "received notification for {:?} {:?}", notif, data);

                        let preamble = VendorDependentPreamble::decode(data)
                            .map_err(|e| Error::PacketError(e))?;

                        let data = &data[preamble.encoded_len()..];

                        if data.len() < preamble.parameter_length as usize {
                            return Err(Error::UnexpectedResponse);
                        }

                        match notif {
                            NotificationEventId::EventPlaybackStatusChanged => {
                                let response =
                                    PlaybackStatusChangedNotificationResponse::decode(data)
                                        .map_err(|e| Error::PacketError(e))?;
                                peer.broadcast_event(PeerControllerEvent::PlaybackStatusChanged(
                                    response.playback_status(),
                                ));
                                Ok(false)
                            }
                            NotificationEventId::EventTrackChanged => {
                                let response = TrackChangedNotificationResponse::decode(data)
                                    .map_err(|e| Error::PacketError(e))?;
                                peer.broadcast_event(PeerControllerEvent::TrackIdChanged(
                                    response.identifier(),
                                ));
                                Ok(false)
                            }
                            NotificationEventId::EventPlaybackPosChanged => {
                                let response = PlaybackPosChangedNotificationResponse::decode(data)
                                    .map_err(|e| Error::PacketError(e))?;
                                peer.broadcast_event(PeerControllerEvent::PlaybackPosChanged(
                                    response.position(),
                                ));
                                Ok(false)
                            }
                            NotificationEventId::EventVolumeChanged => {
                                let response = VolumeChangedNotificationResponse::decode(data)
                                    .map_err(|e| Error::PacketError(e))?;
                                peer.broadcast_event(PeerControllerEvent::VolumeChanged(
                                    response.volume(),
                                ));
                                Ok(false)
                            }
                            _ => Ok(true),
                        }
                    }

                    loop {
                        futures::select! {
                            event_result = stream.select_next_some() => {
                                match event_result {
                                    Ok(data) => {
                                        if handle_response(&notif, &peer, &data[..])
                                            .unwrap_or_else(|e| { fx_log_err!("Error decoding packet from peer {:?}", e); true} )  {
                                            break true;
                                        }
                                        // no error we keep pumping the stream
                                    },
                                    Err(Error::CommandNotSupported) => break false,
                                    Err(_) => break true,
                                    _=> break true,
                                }
                            }
                            complete => { break true; }
                        }
                    }
                };

                notifications.push(fut);
            }

            while let Some(stop) = await!(notifications.next()) {
                // stopping pumping if one of the notification streams had an unrecoverable error
                if stop {
                    break;
                }
            }
            fx_vlog!(tag: "avrcp", 2, "stopping notifications for {:#?}", shared_peer_id);
        });
    }

    fn handle_control_channel_command(
        &self,
        peer_id: &PeerId,
        command: Result<AvcCommand, bt_avctp::Error>,
    ) {
        let remote_peer = self.inner.get_remote_peer(peer_id);
        let mut close_connection = false;

        match command {
            Ok(avcommand) => {
                // scope lock
                if let Some(cmd_handler) = remote_peer.command_handler.lock().as_ref() {
                    if let Err(e) = cmd_handler.handle_command(avcommand, self.inner.clone()) {
                        fx_log_err!("control channel command handler error {:?}", e);
                        close_connection = true;
                    }
                }
            }
            Err(avctp_error) => {
                fx_log_err!("received error from control channel {:?}", avctp_error);
                close_connection = true;
            }
        }
        if close_connection {
            remote_peer.reset_connection();
        }
    }

    pub async fn run(&mut self) -> Result<(), failure::Error> {
        let mut profile_evt = self.inner.profile_svc.take_event_stream().fuse();
        loop {
            futures::select! {
                _ = self.event_pump_futures.select_next_some() => {},
                (peer_id, command) = self.control_channel_streams.select_next_some() => {
                    self.handle_control_channel_command(&peer_id, command);
                },
                request = self.peer_request.select_next_some() => {
                    let peer_controller =
                        PeerController { peer_id: request.peer_id.clone(), inner: self.inner.clone() };
                    // ignoring error if we failed to reply.
                    let _ = request.reply.send(peer_controller);
                },
                evt = profile_evt.select_next_some() => {
                    self.handle_profile_service_event(evt.map_err(|e| {
                            fx_log_err!("profile service error {:?}", e);
                            FailureError::from(e)
                        })?
                    );
                },
                result = self.new_control_connection_futures.select_next_some() => {
                    match result {
                        Ok((peer_id, socket)) => {
                            self.new_control_connection(&peer_id, socket);
                            self.connect_to_peer_if_necessary(&peer_id);
                        }
                        Err(e) => {
                            fx_log_err!("connection error {:?}", e);
                        }
                    }
                }
            }
        }
    }
}

#[derive(Debug)]
struct PeerManagerInner {
    profile_svc: AvrcpProfile,
    remotes: RwLock<RemotePeersMap>,
    // Contains the current backend for the target handler. Default impl speaks to media session.
    // This may get replaced at runtime with a test target implementation.
    //target_handler: Mutex<Option<Box<TargetHandler>>>,
}

impl PeerManagerInner {
    fn new(profile_svc: AvrcpProfile) -> Self {
        Self { profile_svc, remotes: RwLock::new(HashMap::new()) }
    }

    fn insert_if_needed(&self, peer_id: &str) {
        let mut r = self.remotes.write();
        if !r.contains_key(peer_id) {
            r.insert(String::from(peer_id), Arc::new(RemotePeer::new(String::from(peer_id))));
        }
    }

    fn get_remote_peer(&self, peer_id: &str) -> Arc<RemotePeer> {
        self.insert_if_needed(peer_id);
        self.remotes.read().get(peer_id).unwrap().clone()
    }

    /// Send a typical vendor dependent command and returns it's response where:
    ///     - the command type is "status"
    ///     - you want to ignore interim responses
    ///     - the value is you want back is on the stable response
    ///     - all other response returned as errors
    /// This method encodes the `command` packet, awaits and decodes all responses, will issue
    /// continuation commands for incomplete responses (eg "get_element_attributes" command), and
    /// will return a result of the decoded packet or an error for any non stable response received
    async fn send_vendor_dependent_command<'a>(
        peer: &'a AvcPeer,
        command: &'a impl VendorDependent,
    ) -> Result<Vec<u8>, Error> {
        let mut buf = vec![];
        let packet = command.encode_packet().expect("unable to encode packet");
        let mut stream = peer.send_vendor_dependent_command(AvcCommandType::Status, &packet[..])?;

        loop {
            let response = loop {
                if let Some(result) = await!(stream.next()) {
                    let response: AvcCommandResponse = result.map_err(|e| Error::AvctpError(e))?;
                    fx_vlog!(tag: "avrcp", 1, "vendor response {:#?}", response);
                    match response.response_type() {
                        AvcResponseType::Interim => continue,
                        AvcResponseType::NotImplemented => return Err(Error::CommandNotSupported),
                        AvcResponseType::Rejected => return Err(Error::CommandFailed),
                        AvcResponseType::InTransition => return Err(Error::UnexpectedResponse),
                        AvcResponseType::Changed => return Err(Error::UnexpectedResponse),
                        AvcResponseType::Accepted => return Err(Error::UnexpectedResponse),
                        AvcResponseType::ImplementedStable => break response.1,
                    }
                } else {
                    return Err(Error::CommandFailed);
                }
            };

            match VendorDependentPreamble::decode(&response[..]) {
                Ok(preamble) => {
                    buf.extend_from_slice(&response[preamble.encoded_len()..]);
                    match preamble.packet_type() {
                        PacketType::Single | PacketType::Stop => {
                            break;
                        }
                        // Still more to decode. Queue up a continuation call.
                        _ => {}
                    }
                }
                Err(e) => {
                    fx_log_info!("Unable to parse vendor dependent preamble: {:?}", e);
                    return Err(Error::PacketError(e));
                }
            };

            let packet = RequestContinuingResponseCommand::new(u8::from(&command.pdu_id()))
                .encode_packet()
                .expect("unable to encode packet");

            stream = peer.send_vendor_dependent_command(AvcCommandType::Control, &packet[..])?;
        }
        Ok(buf)
    }

    async fn send_avc_passthrough_keypress<'a>(
        &'a self,
        peer_id: &'a str,
        avc_keycode: u8,
    ) -> Result<(), Error> {
        let remote = self.get_remote_peer(peer_id);
        {
            // key_press
            let payload_1 = &[avc_keycode, 0x00];
            let r = remote.control_channel.read().connection();
            if let Some(peer) = r {
                let response = await!(peer.send_avc_passthrough_command(payload_1));
                match response {
                    Ok(AvcCommandResponse(AvcResponseType::Accepted, _)) => {}
                    Ok(AvcCommandResponse(AvcResponseType::NotImplemented, _)) => {
                        return Err(Error::CommandNotSupported);
                    }
                    Err(e) => {
                        fx_log_err!("error sending avc command to {}: {:?}", peer_id, e);
                        return Err(Error::CommandFailed);
                    }
                    Ok(response) => {
                        fx_log_err!(
                            "error sending avc command. unhandled response {}: {:?}",
                            peer_id,
                            response
                        );
                        return Err(Error::CommandFailed);
                    }
                }
            } else {
                return Err(Error::RemoteNotFound);
            }
        }
        {
            // key_release
            let payload_2 = &[avc_keycode | 0x80, 0x00];
            let r = remote.control_channel.read().connection();
            if let Some(peer) = r {
                let response = await!(peer.send_avc_passthrough_command(payload_2));
                match response {
                    Ok(AvcCommandResponse(AvcResponseType::Accepted, _)) => {
                        return Ok(());
                    }
                    Ok(AvcCommandResponse(AvcResponseType::Rejected, _)) => {
                        fx_log_info!("avrcp command rejected {}: {:?}", peer_id, response);
                        return Err(Error::CommandNotSupported);
                    }
                    Err(e) => {
                        fx_log_err!("error sending avc command to {}: {:?}", peer_id, e);
                        return Err(Error::CommandFailed);
                    }
                    _ => {
                        fx_log_err!(
                            "error sending avc command. unhandled response {}: {:?}",
                            peer_id,
                            response
                        );
                        return Err(Error::CommandFailed);
                    }
                }
            } else {
                Err(Error::RemoteNotFound)
            }
        }
    }

    async fn get_media_attributes<'a>(
        &'a self,
        peer_id: &'a PeerId,
    ) -> Result<MediaAttributes, Error> {
        let remote = self.get_remote_peer(peer_id);
        let conn = remote.control_channel.read().connection();
        match conn {
            Some(peer) => {
                let mut media_attributes = MediaAttributes::new_empty();
                let cmd = GetElementAttributesCommand::all_attributes();
                fx_vlog!(tag: "avrcp", 1, "get_media_attributes send command {:#?}", cmd);
                let buf = await!(Self::send_vendor_dependent_command(&peer, &cmd))?;
                let response = GetElementAttributesResponse::decode(&buf[..])
                    .map_err(|e| Error::PacketError(e))?;
                fx_vlog!(tag: "avrcp", 1, "get_media_attributes received response {:#?}", response);
                media_attributes.title = response.title.unwrap_or("".to_string());
                media_attributes.artist_name = response.artist_name.unwrap_or("".to_string());
                media_attributes.album_name = response.album_name.unwrap_or("".to_string());
                media_attributes.track_number = response.track_number.unwrap_or("".to_string());
                media_attributes.total_number_of_tracks =
                    response.total_number_of_tracks.unwrap_or("".to_string());
                media_attributes.genre = response.genre.unwrap_or("".to_string());
                media_attributes.playing_time = response.playing_time.unwrap_or("".to_string());
                Ok(media_attributes)
            }
            _ => Err(Error::RemoteNotFound),
        }
    }

    async fn get_supported_events<'a>(
        &'a self,
        peer_id: &'a PeerId,
    ) -> Result<Vec<NotificationEventId>, Error> {
        let remote = self.get_remote_peer(peer_id);
        let conn = remote.control_channel.read().connection();
        match conn {
            Some(peer) => {
                let cmd = GetCapabilitiesCommand::new(GetCapabilitiesCapabilityId::EventsId);
                fx_vlog!(tag: "avrcp", 1, "get_capabilities(events) send command {:#?}", cmd);
                let buf = await!(Self::send_vendor_dependent_command(&peer, &cmd))?;
                let capabilities =
                    GetCapabilitiesResponse::decode(&buf[..]).map_err(|e| Error::PacketError(e))?;
                let mut event_ids = vec![];
                for event_id in capabilities.event_ids() {
                    event_ids.push(NotificationEventId::try_from(event_id)?);
                }
                Ok(event_ids)
            }
            _ => Err(Error::RemoteNotFound),
        }
    }
}

#[derive(Debug, Clone)]
pub enum PeerControllerEvent {
    PlaybackStatusChanged(PlaybackStatus),
    TrackIdChanged(u64),
    PlaybackPosChanged(u32),
    VolumeChanged(u8),
}

pub type PeerControllerEventStream = mpsc::Receiver<PeerControllerEvent>;

/// Handed back to the service client end point to control a specific peer.
#[derive(Debug)]
pub struct PeerController {
    inner: Arc<PeerManagerInner>,
    peer_id: PeerId,
}

impl PeerController {
    pub fn is_connected(&self) -> bool {
        let remote = self.inner.get_remote_peer(&self.peer_id);
        let connection = remote.control_channel.read();
        match *connection {
            PeerChannel::Connected(_) => true,
            _ => false,
        }
    }

    pub async fn send_avc_passthrough_keypress(&self, avc_keycode: u8) -> Result<(), Error> {
        await!(self.inner.send_avc_passthrough_keypress(&self.peer_id, avc_keycode))
    }

    pub async fn get_media_attributes(&self) -> Result<MediaAttributes, Error> {
        await!(self.inner.get_media_attributes(&self.peer_id))
    }

    pub async fn get_supported_events(&self) -> Result<Vec<NotificationEventId>, Error> {
        await!(self.inner.get_supported_events(&self.peer_id))
    }

    pub async fn send_raw_vendor_command<'a>(
        &'a self,
        pdu_id: u8,
        payload: &'a [u8],
    ) -> Result<Vec<u8>, Error> {
        let command = RawVendorDependentPacket::new(PduId::try_from(pdu_id)?, payload);
        let remote = self.inner.get_remote_peer(&self.peer_id);
        let connection = remote.control_channel.read().connection();
        match connection {
            Some(peer) => await!(PeerManagerInner::send_vendor_dependent_command(&peer, &command)),
            _ => Err(Error::RemoteNotFound),
        }
    }

    pub fn take_event_stream(&self) -> PeerControllerEventStream {
        let (sender, receiver) = mpsc::channel(512);
        let remote = self.inner.get_remote_peer(&self.peer_id);
        remote.controller_listeners.lock().push(sender);
        receiver
    }
}

/// Handles commands from the command stream for a control channel and maintains state for the peer
#[derive(Debug)]
struct ControlChannelCommandHandler {
    /// Handle back to the remote peer. Weak to prevent a reference cycle since the remote peer owns this object.
    remote_peer: Weak<RemotePeer>,
    // TODO: implement
    // Stores the remaining packets packets as part of fragmented response.
    // Map of PduIds to a Vec of remaining encoded packets.
    //#[allow(dead_code)]
    //continuations: HashMap<u8, Vec<Vec<u8>>>,
}

impl ControlChannelCommandHandler {
    fn new(remote_peer: Weak<RemotePeer>) -> Self {
        Self { remote_peer }
    }

    fn handle_passthrough_command(
        &self,
        _inner: Arc<PeerManagerInner>,
        command: &AvcCommand,
    ) -> Result<AvcResponseType, Error> {
        let body = command.body();
        let command = AvcPanelCommand::from_primitive(body[0]);

        if let Some(peer) = Weak::upgrade(&self.remote_peer) {
            fx_log_info!("Received passthrough command {:x?} {}", command, &peer.peer_id);
        }

        match command {
            Some(_) => Ok(AvcResponseType::Accepted),
            None => Ok(AvcResponseType::Rejected),
        }
    }

    fn assemble_vendor_response(&self, response: impl VendorDependent) -> Result<Vec<u8>, Error> {
        response.encode_packet().map_err(|e| Error::PacketError(e))
    }

    fn handle_notification(&self, _inner: Arc<PeerManagerInner>, command: AvcCommand) {
        let packet_body = command.body();

        let preamble = match VendorDependentPreamble::decode(packet_body) {
            Err(e) => {
                if let Some(remote_peer) = Weak::upgrade(&self.remote_peer) {
                    fx_log_info!(
                        "Unable to parse vendor dependent preamble {}: {:?}",
                        remote_peer.peer_id,
                        e
                    );
                }
                let _ = command.send_response(AvcResponseType::NotImplemented, packet_body);
                return;
            }
            Ok(x) => x,
        };

        let body = &packet_body[preamble.encoded_len()..];

        let pdu_id = match PduId::try_from(preamble.pdu_id) {
            Err(e) => {
                if let Some(remote_peer) = Weak::upgrade(&self.remote_peer) {
                    fx_log_err!(
                        "Unsupported vendor dependent command pdu {} received from peer {} {:#?}: {:?}",
                        preamble.pdu_id,
                        remote_peer.peer_id,
                        body,
                        e
                    );
                }
                let _ = command.send_response(AvcResponseType::NotImplemented, packet_body);
                return;
            }
            Ok(x) => x,
        };

        if pdu_id != PduId::RegisterNotification {
            let reject_response = RejectResponse::new(&pdu_id, &StatusCode::InvalidParameter);
            let packet = reject_response.encode_packet().unwrap();
            let _ = command.send_response(AvcResponseType::Rejected, &packet[..]);
            return;
        }

        match RegisterNotificationCommand::decode(&body[..]) {
            Ok(notification_command) => match *notification_command.event_id() {
                /*NotificationEventId::EventPlaybackStatusChanged => {
                    let response =
                        PlaybackStatusChangedNotificationResponse::new(PlaybackStatus::Stopped);
                    match response.encode_packet() {
                        Ok(packet) => {
                            let _ = command.send_response(AvcResponseType::Interim, &packet[..]);
                        }
                        Err(_) => {
                            let _ = command.send_response(AvcResponseType::NotImplemented, &[]);
                            return;
                        }
                    }

                    fasync::spawn(fasync::Timer::new(10.seconds().after_now()).then(move |()| {
                        let response =
                            PlaybackStatusChangedNotificationResponse::new(PlaybackStatus::Playing);
                        match response.encode_packet() {
                            Ok(packet) => {
                                let _ =
                                    command.send_response(AvcResponseType::Changed, &packet[..]);
                            }
                            Err(_) => {
                                let _ = command.send_response(AvcResponseType::Rejected, &[]);
                                return;
                            }
                        }
                    }));

                    return;
                }
                NotificationEventId::EventTrackChanged => {
                    let response = TrackChangedNotificationResponse::none();
                    match response.encode_packet() {
                        Ok(packet) => {
                            let _ = command.send_response(AvcResponseType::Interim, &packet[..]);
                        }
                        Err(_) => {
                            let _ = command.send_response(AvcResponseType::NotImplemented, &[]);
                            return;
                        }
                    }

                    fasync::spawn(fasync::Timer::new(10.seconds().after_now()).then(move |()| {
                        let response = TrackChangedNotificationResponse::unknown();
                        match response.encode_packet() {
                            Ok(packet) => {
                                let _ =
                                    command.send_response(AvcResponseType::Changed, &packet[..]);
                            }
                            Err(_) => {
                                let _ = command.send_response(AvcResponseType::Rejected, &[]);
                                return;
                            }
                        }
                    }));
                    return;
                }
                NotificationEventId::EventPlaybackPosChanged => {
                    let response = PlaybackPosChangedNotificationResponse::new(0x30);
                    match response.encode_packet() {
                        Ok(packet) => {
                            let _ = command.send_response(AvcResponseType::Interim, &packet[..]);
                        }
                        Err(_) => {
                            let _ = command.send_response(AvcResponseType::NotImplemented, &[]);
                            return;
                        }
                    }

                    fasync::spawn(fasync::Timer::new(10.seconds().after_now()).then(move |()| {
                        let response = PlaybackPosChangedNotificationResponse::new(0x31);
                        match response.encode_packet() {
                            Ok(packet) => {
                                let _ =
                                    command.send_response(AvcResponseType::Changed, &packet[..]);
                            }
                            Err(_) => {
                                let _ = command.send_response(AvcResponseType::Rejected, &[]);
                                return;
                            }
                        }
                    }));
                    return;
                }
                NotificationEventId::EventVolumeChanged => {
                    let response = VolumeChangedNotificationResponse::new(0x30);
                    match response.encode_packet() {
                        Ok(packet) => {
                            let _ = command.send_response(AvcResponseType::Interim, &packet[..]);
                        }
                        Err(_) => {
                            let _ = command.send_response(AvcResponseType::NotImplemented, &[]);
                            return;
                        }
                    }
                    fasync::spawn(fasync::Timer::new(10.seconds().after_now()).then(move |()| {
                        let response = VolumeChangedNotificationResponse::new(0x31);
                        match response.encode_packet() {
                            Ok(packet) => {
                                let _ =
                                    command.send_response(AvcResponseType::Changed, &packet[..]);
                            }
                            Err(_) => {
                                let _ = command.send_response(AvcResponseType::Rejected, &[]);
                                return;
                            }
                        }
                    }));
                    return;
                }*/
                _ => {
                    let reject_response = RejectResponse::new(&PduId::RegisterNotification, &StatusCode::InvalidParameter);
                    let packet = reject_response.encode_packet().unwrap();
                    let _ = command.send_response(AvcResponseType::Rejected, &packet[..]);
                    return;
                }
            },
            Err(e) => {
                fx_log_err!(
                    "Unable to decode register notification command {} {:#?}: {:?}",
                    preamble.pdu_id,
                    body,
                    e
                );
                let reject_response = RejectResponse::new(&PduId::RegisterNotification, &StatusCode::InvalidCommand);
                let packet = reject_response.encode_packet().unwrap();
                let _ = command.send_response(AvcResponseType::Rejected, &packet[..]);
                return;
            }
        }
    }

    fn handle_vendor_command(
        &self,
        _inner: Arc<PeerManagerInner>,
        pdu_id: PduId,
        body: &[u8],
    ) -> Result<(AvcResponseType, Vec<u8>), Error> {
        match pdu_id {
            PduId::GetCapabilities => {
                let get_cap_cmd =
                    GetCapabilitiesCommand::decode(body).map_err(|e| Error::PacketError(e))?;

                fx_vlog!(tag: "avrcp", 2, "Received GetCapabilities Command {:#?}", get_cap_cmd);

                match get_cap_cmd.capability_id() {
                    GetCapabilitiesCapabilityId::CompanyId => {
                        let response = GetCapabilitiesResponse::new_btsig_company();
                        let buf = self.assemble_vendor_response(response)?;
                        Ok((AvcResponseType::ImplementedStable, buf))
                    }
                    GetCapabilitiesCapabilityId::EventsId => {
                        let response = GetCapabilitiesResponse::new_events(&[
                            //u8::from(&NotificationEventId::EventVolumeChanged),
                            //u8::from(&NotificationEventId::EventPlaybackStatusChanged),
                            //u8::from(&NotificationEventId::EventTrackChanged),
                            //u8::from(&NotificationEventId::EventPlaybackPosChanged),
                        ]);
                        let buf = self.assemble_vendor_response(response)?;
                        Ok((AvcResponseType::ImplementedStable, buf))
                    }
                }
            }
            PduId::GetElementAttributes => {
                let get_element_attrib_command =
                    GetElementAttributesCommand::decode(body).map_err(|e| Error::PacketError(e))?;

                fx_vlog!(tag: "avrcp", 2, "Received GetElementAttributes Command {:#?}", get_element_attrib_command);
                let response = GetElementAttributesResponse {
                    title: Some("Hello world".to_string()),
                    ..GetElementAttributesResponse::default()
                };
                let buf = self.assemble_vendor_response(response)?;
                Ok((AvcResponseType::ImplementedStable, buf))
            }
            _ => Err(Error::CommandNotSupported),
        }
    }

    fn handle_command(&self, command: AvcCommand, pmi: Arc<PeerManagerInner>) -> Result<(), Error> {
        if let Some(remote_peer) = Weak::upgrade(&self.remote_peer) {
            fx_vlog!(tag: "avrcp", 2, "received command {:#?}", command);
            if command.is_vendor_dependent() {
                let packet_body = command.body();

                let preamble = match VendorDependentPreamble::decode(packet_body) {
                    Err(e) => {
                        if let Some(remote_peer) = Weak::upgrade(&self.remote_peer) {
                            fx_log_info!(
                                "Unable to parse vendor dependent preamble {}: {:?}",
                                remote_peer.peer_id,
                                e
                            );
                        }
                        let _ = command.send_response(AvcResponseType::NotImplemented, &packet_body[..]);
                        return Ok(());
                    }
                    Ok(x) => x,
                };

                let body = &packet_body[preamble.encoded_len()..];

                let pdu_id = match PduId::try_from(preamble.pdu_id) {
                    Err(e) => {
                        if let Some(remote_peer) = Weak::upgrade(&self.remote_peer) {
                            fx_log_err!(
                                "Unsupported vendor dependent command pdu {} received from peer {} {:#?}: {:?}",
                                preamble.pdu_id,
                                remote_peer.peer_id,
                                body,
                                e
                            );
                        }
                        // recoverable error
                        let _ = command.send_response(AvcResponseType::NotImplemented, &packet_body[..]);
                        return Ok(());
                    }
                    Ok(x) => x,
                };
                fx_vlog!(tag: "avrcp", 2, "Received command PDU {:#?}", pdu_id);

                match command.avc_header().packet_type() {
                    AvcPacketType::Command(AvcCommandType::Notify) => {
                        self.handle_notification(pmi, command);
                        Ok(())
                    }
                    AvcPacketType::Command(AvcCommandType::Control) => {
                        match self.handle_vendor_command(pmi, pdu_id, &body[..]) {
                            Ok((response_type, buf)) => {
                                if let Err(e) = command.send_response(response_type, &buf[..]) {
                                    fx_log_err!(
                                        "Error sending vendor response to peer {}, {:?}",
                                        remote_peer.peer_id,
                                        e
                                    );
                                    return Err(Error::from(e));
                                }
                                Ok(())
                            }
                            Err(e) => {
                                fx_log_err!(
                                    "Unrecoverable error parsing command packet from peer {}, {:?}",
                                    remote_peer.peer_id,
                                    e
                                );

                                match e {
                                    Error::CommandNotSupported => {
                                        if let Err(e) = command.send_response(AvcResponseType::NotImplemented, &packet_body[..]) {
                                            fx_log_err!(
                                                "Error sending not implemented response to peer {}, {:?}",
                                                remote_peer.peer_id,
                                                e
                                            );
                                            return Err(Error::from(e));
                                        }
                                        Ok(())
                                    }
                                    _=> {
                                        let response_error_code = match e {
                                            Error::PacketError(PacketError::OutOfRange) => StatusCode::InvalidParameter,
                                            Error::PacketError(PacketError::InvalidHeader) => StatusCode::ParameterContentError,
                                            Error::PacketError(PacketError::InvalidMessage) => StatusCode::ParameterContentError,
                                            Error::PacketError(PacketError::UnsupportedMessage) => StatusCode::InternalError,
                                            _ => StatusCode::InternalError,
                                        };

                                        let reject_response = RejectResponse::new(&PduId::RegisterNotification, &response_error_code);
                                        if let Ok(packet) = reject_response.encode_packet() {
                                            if let Err(e) = command.send_response(AvcResponseType::Rejected, &packet[..]) {
                                                fx_log_err!(
                                                    "Error sending vendor reject response to peer {}, {:?}",
                                                    remote_peer.peer_id,
                                                    e
                                                );
                                                return Err(Error::from(e));
                                            }
                                        } else {
                                            // Todo handle this case better.
                                            fx_log_err!("Unable to encoded reject response");
                                        }
                                        Ok(())
                                    }
                                }


                            }
                        }
                    }
                    _ => {
                        let _ = command.send_response(AvcResponseType::NotImplemented, &packet_body[..]);
                        Ok(())
                    }
                }
            } else {
                match self.handle_passthrough_command(pmi, &command) {
                    Ok(response_type) => {
                        if let Err(e) = command.send_response(response_type, &[]) {
                            fx_log_err!(
                                "Error sending passthrough response to peer {}, {:?}",
                                remote_peer.peer_id,
                                e
                            );
                            return Err(Error::from(e));
                        }
                        Ok(())
                    }
                    Err(e) => {
                        fx_log_err!(
                            "Unrecoverable error parsing command packet from peer {}, {:?}",
                            remote_peer.peer_id,
                            e
                        );
                        let _ = command.send_response(AvcResponseType::Rejected, &[]);
                        Err(e)
                    }
                }
            }
        } else {
            panic!("Unexpected state. remote peer should not be deallocated")
        }
    }
}

#[allow(dead_code)]
struct NotificationStream {
    peer: Arc<RemotePeer>,
    peer_id: PeerId,
    event_id: NotificationEventId,
    playback_interval: u32,
    stream: Option<Pin<Box<Stream<Item = Result<AvcCommandResponse, AvctpError>>>>>,
    terminated: bool,
}

impl NotificationStream {
    fn new(
        peer: Arc<RemotePeer>,
        peer_id: &PeerId,
        event_id: NotificationEventId,
        playback_interval: u32,
    ) -> Self {
        Self {
            peer,
            peer_id: peer_id.clone(),
            event_id: event_id.clone(),
            playback_interval,
            stream: None,
            terminated: false,
        }
    }

    fn setup_stream(
        &self,
    ) -> Result<impl Stream<Item = Result<AvcCommandResponse, AvctpError>>, Error> {
        let command = if self.event_id == NotificationEventId::EventPlaybackPosChanged {
            RegisterNotificationCommand::new_position_changed(self.playback_interval)
        } else {
            RegisterNotificationCommand::new(self.event_id)
        };
        let conn = self.peer.control_channel.read().connection().ok_or(Error::RemoteNotFound)?;
        let packet = command.encode_packet().expect("unable to encode packet");
        Ok(conn
            .send_vendor_dependent_command(AvcCommandType::Notify, &packet[..])
            .map_err(|e| Error::from(e))?)
    }
}

impl FusedStream for NotificationStream {
    fn is_terminated(&self) -> bool {
        self.terminated
    }
}

impl Stream for NotificationStream {
    type Item = Result<Vec<u8>, Error>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let mut this = self;

        if this.terminated {
            return Poll::Ready(None);
        }

        loop {
            if this.stream.is_none() {
                match this.setup_stream() {
                    Ok(stream) => this.stream = Some(Box::pin(stream)),
                    Err(e) => {
                        this.terminated = true;
                        return Poll::Ready(Some(Err(e)));
                    }
                }
            }

            let stream_box = this.stream.as_mut().unwrap();
            let stream = stream_box.as_mut();
            let result = ready!(stream.poll_next(cx));
            match result {
                Some(Ok(response)) => {
                    fx_vlog!(tag: "avrcp", 2, "received event response {:?}", response);
                    // We ignore the "changed" event and just use it to let use requeue a new
                    // register notification. We will then just use the interim response of the next
                    // command to prevent duplicates. "rejected" after interim typically happens
                    // when the player has changed so we re-prime the notification again just like
                    // a changed event.
                    match response.response_type() {
                        AvcResponseType::Interim => {
                            return Poll::Ready(Some(Ok(response.response().to_vec())))
                        }
                        AvcResponseType::NotImplemented => {
                            this.terminated = true;
                            return Poll::Ready(Some(Err(Error::CommandNotSupported)));
                        }
                        AvcResponseType::Rejected => {
                            let body = response.response();
                            if let Ok(reject_packet) = VendorDependentPreamble::decode(&body[..]) {
                                let payload = &body[reject_packet.encoded_len()..];
                                if payload.len() > 0 {
                                    if let Ok(status_code) = StatusCode::try_from(payload[0]) {
                                        match status_code {
                                            StatusCode::AddressedPlayerChanged => {
                                                this.stream = None;
                                                continue;
                                            }
                                            StatusCode::InvalidCommand => {
                                                this.terminated = true;
                                                return Poll::Ready(Some(Err(Error::CommandFailed)));
                                            }
                                            StatusCode::InvalidParameter => {
                                                this.terminated = true;
                                                return Poll::Ready(Some(Err(Error::CommandNotSupported)));
                                            }
                                            StatusCode::InternalError => {
                                                this.terminated = true;
                                                return Poll::Ready(Some(Err(Error::GenericError(format_err!("Remote internal error")))));
                                            }
                                            _ => {}
                                        }
                                    }
                                }
                            }
                            this.terminated = true;
                            return Poll::Ready(Some(Err(Error::UnexpectedResponse)));
                        }
                        AvcResponseType::Changed => {
                            // Repump.
                            this.stream = None;
                        }
                        // All others are invalid responses for register notification.
                        _ => {
                            this.terminated = true;
                            return Poll::Ready(Some(Err(Error::UnexpectedResponse)));
                        }
                    }
                }
                Some(Err(e)) => {
                    this.terminated = true;
                    return Poll::Ready(Some(Err(Error::from(e))));
                }
                None => {
                    this.terminated = true;
                    return Poll::Ready(None);
                }
            }
        }
    }
}
