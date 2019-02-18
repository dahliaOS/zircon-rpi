// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::Error,
    fidl::encoding::Decodable as FidlDecodable,
    fidl::endpoints::{RequestStream, ServiceMarker},
    fidl_fuchsia_bluetooth_avrcp::*,
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::{self, fx_log_err, fx_log_info, fx_log_warn},
    fuchsia_zircon as zx,
    futures::{
        self, channel::mpsc, future::FutureExt, future::TryFutureExt, stream::StreamExt,
        stream::TryStreamExt, Future,
    },
};

use crate::packets::PlaybackStatus as PacketPlaybackStatus;
use crate::peer::{PeerController, PeerControllerEvent, PeerControllerRequest};
use crate::types::PeerError;

impl From<PeerError> for ControllerError {
    fn from(e: PeerError) -> Self {
        match e {
            PeerError::PacketError(_) => ControllerError::PacketEncoding,
            PeerError::AvctpError(_) => ControllerError::ProtocolError,
            PeerError::RemoteNotFound => ControllerError::RemoteNotConnected,
            PeerError::CommandNotSupported => ControllerError::CommandNotImplemented,
            PeerError::CommandFailed => ControllerError::UnkownFailure,
            PeerError::ConnectionFailure(_) => ControllerError::ConnectionError,
            PeerError::UnexpectedResponse => ControllerError::UnexpectedResponse,
            _ => ControllerError::UnkownFailure,
        }
    }
}

struct AvrcpClientController {
    controller: PeerController,
    fidl_stream: ControllerRequestStream,
}

impl AvrcpClientController {
    async fn handle_fidl_request(&self, request: ControllerRequest) -> Result<(), Error> {
        match request {
            ControllerRequest::GetPlayerApplicationSettings { responder } => {
                responder.send(&mut Err(ControllerError::CommandNotImplemented))?;
            }
            ControllerRequest::GetMediaAttributes { responder } => {
                responder.send(
                    &mut await!(self.controller.get_media_attributes())
                        .map_err(ControllerError::from),
                )?;
            }
            ControllerRequest::SetAbsoluteVolume { volume: _, responder } => {
                responder.send(&mut Err(ControllerError::CommandNotImplemented))?;
            }
            ControllerRequest::InformBatteryStatus { battery_status: _, responder } => {
                responder.send(&mut Err(ControllerError::CommandNotImplemented))?;
            }
            ControllerRequest::SetAddressedPlayer { player_id: _, responder } => {
                responder.send(&mut Err(ControllerError::CommandNotImplemented))?;
            }
            ControllerRequest::SendCommand { command, responder } => {
                responder.send(
                    &mut await!(self
                        .controller
                        .send_avc_passthrough_keypress(command.into_primitive()))
                    .map_err(ControllerError::from),
                )?;
            }
        };
        Ok(())
    }

    fn handle_controller_event(&self, event: PeerControllerEvent) -> Result<(), Error> {
        let control_handle: ControllerControlHandle = self.fidl_stream.control_handle();

        match event {
            PeerControllerEvent::PlaybackStatusChanged(playback_status) => {
                let status = match playback_status {
                    PacketPlaybackStatus::Stopped => PlaybackStatus::Stopped,
                    PacketPlaybackStatus::Playing => PlaybackStatus::Playing,
                    PacketPlaybackStatus::Paused => PlaybackStatus::Paused,
                    PacketPlaybackStatus::FwdSeek => PlaybackStatus::FwdSeek,
                    PacketPlaybackStatus::RevSeek => PlaybackStatus::RevSeek,
                    PacketPlaybackStatus::Error => PlaybackStatus::Error,
                };
                let notification = Notification {
                    timestamp: Some(zx::Time::get(zx::ClockId::UTC).into_nanos()),
                    status: Some(status),
                    ..Notification::new_empty()
                };
                control_handle.send_on_notification(notification).map_err(Error::from)
            }
            PeerControllerEvent::TrackIdChanged(track_id) => {
                let notification = Notification {
                    timestamp: Some(zx::Time::get(zx::ClockId::UTC).into_nanos()),
                    track_id: Some(track_id),
                    ..Notification::new_empty()
                };
                control_handle.send_on_notification(notification).map_err(Error::from)
            }
            PeerControllerEvent::PlaybackPosChanged(pos) => {
                let notification = Notification {
                    timestamp: Some(zx::Time::get(zx::ClockId::UTC).into_nanos()),
                    pos: Some(pos),
                    ..Notification::new_empty()
                };
                control_handle.send_on_notification(notification).map_err(Error::from)
            }
            PeerControllerEvent::VolumeChanged(volume) => {
                let notification = Notification {
                    timestamp: Some(zx::Time::get(zx::ClockId::UTC).into_nanos()),
                    volume: Some(volume),
                    ..Notification::new_empty()
                };
                control_handle.send_on_notification(notification).map_err(Error::from)
            }
        }
    }

    async fn run(&mut self) -> Result<(), Error> {
        let mut controller_events = self.controller.take_event_stream();
        loop {
            futures::select! {
                req = self.fidl_stream.select_next_some() => {
                    await!(self.handle_fidl_request(req?))?;
                }
                cmd = controller_events.select_next_some() => {
                    self.handle_controller_event(cmd)?;
                }
                complete => { return Ok(()); }
            }
        }
    }
}

struct TestAvrcpClientController {
    controller: PeerController,
    fidl_stream: TestControllerRequestStream,
}

impl TestAvrcpClientController {
    async fn handle_fidl_request(&self, request: TestControllerRequest) -> Result<(), Error> {
        match request {
            TestControllerRequest::IsConnected { responder } => {
                responder.send(self.controller.is_connected())?;
            }
            TestControllerRequest::GetEventsSupported { responder } => {
                match await!(self.controller.get_supported_events()) {
                    Ok(events) => {
                        let mut r_events = vec![];
                        for e in events {
                            if let Some(target_event) = TargetEvent::from_primitive(u8::from(&e)) {
                                r_events.push(target_event);
                            }
                        }
                        responder.send(&mut Ok(r_events))?;
                    }
                    Err(peer_error) => {
                        responder.send(&mut Err(ControllerError::from(peer_error)))?
                    }
                }
            }
            TestControllerRequest::Connect { control_handle: _ } => {
                // TODO
            }
            TestControllerRequest::Disconnect { control_handle: _ } => {
                // TODO
            }
            TestControllerRequest::SendRawVendorDependentCommand { pdu_id, command, responder } => {
                responder.send(&mut await!(self
                    .controller
                    .send_raw_vendor_command(pdu_id, &command[..])
                    .map_err(|e| ControllerError::from(e))))?;
            }
        };
        Ok(())
    }

    async fn run(&mut self) -> Result<(), Error> {
        loop {
            futures::select! {
                req = self.fidl_stream.select_next_some() => {
                    await!(self.handle_fidl_request(req?))?;
                }
                complete => { return Ok(()); }
            }
        }
    }
}

fn spawn_avrcp_client_controller(controller: PeerController, fidl_stream: ControllerRequestStream) {
    fasync::spawn(
        async move {
            let mut acc = AvrcpClientController { controller, fidl_stream };
            await!(acc.run())?;
            Ok(())
        }
            .boxed()
            .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e)),
    );
}

fn spawn_test_avrcp_client_controller(
    controller: PeerController,
    fidl_stream: TestControllerRequestStream,
) {
    fasync::spawn(
        async move {
            let mut acc = TestAvrcpClientController { controller, fidl_stream };
            await!(acc.run())?;
            Ok(())
        }
            .boxed()
            .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e)),
    );
}

pub fn spawn_avrcp_client(
    mut stream: AvrcpRequestStream,
    mut sender: mpsc::Sender<PeerControllerRequest>,
) {
    fx_log_info!("Spawning avrcp client handler");
    fasync::spawn(
        async move {
            while let Some(AvrcpRequest::GetControllerForTarget { peer_id, client, responder }) =
                await!(stream.try_next())?
            {
                let client: fidl::endpoints::ServerEnd<ControllerMarker> = client;

                fx_log_info!("New connection request for {}", peer_id);

                match client.into_stream() {
                    Err(err) => {
                        fx_log_warn!("Err unable to create server end point from stream {:?}", err);
                        responder.send(&mut Err(zx::Status::UNAVAILABLE.into_raw()))?;
                    }
                    Ok(client_stream) => {
                        let (response, pcr) = PeerControllerRequest::new(peer_id);
                        sender.try_send(pcr)?;
                        let controller = await!(response.into_future())?;
                        spawn_avrcp_client_controller(controller, client_stream);
                        responder.send(&mut Ok(()))?;
                    }
                }
            }
            Ok(())
        }
            .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e)),
    );
}

pub fn spawn_test_avrcp_client(
    mut stream: TestAvrcpRequestStream,
    mut sender: mpsc::Sender<PeerControllerRequest>,
) {
    fx_log_info!("Spawning test avrcp client handler");
    fasync::spawn(
        async move {
            while let Some(req) = await!(stream.try_next())? {
                match req {
                    TestAvrcpRequest::GetTestControllerForTarget { peer_id, client, responder } => {
                        let client: fidl::endpoints::ServerEnd<TestControllerMarker> = client;

                        fx_log_info!("New connection request for {}", peer_id);

                        match client.into_stream() {
                            Err(err) => {
                                fx_log_warn!(
                                    "Err unable to create server end point from stream {:?}",
                                    err
                                );
                                responder.send(&mut Err(zx::Status::UNAVAILABLE.into_raw()))?;
                            }
                            Ok(client_stream) => {
                                let (response, pcr) = PeerControllerRequest::new(peer_id);
                                sender.try_send(pcr)?;
                                let controller = await!(response.into_future())?;
                                spawn_test_avrcp_client_controller(controller, client_stream);
                                responder.send(&mut Ok(()))?;
                            }
                        }
                    }
                    // TODO: handler support
                    TestAvrcpRequest::RegisterIncomingTargetHandler { .. } => {}
                }
            }
            Ok(())
        }
            .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e)),
    );
}

pub fn run_service(
    sender: mpsc::Sender<PeerControllerRequest>,
) -> Result<(impl Future<Output = ()>), Error> {
    let mut fs = ServiceFs::new();
    let sender_avrcp = sender.clone();
    let sender_test = sender.clone();
    fs.dir("public")
        .add_fidl_service_at(TestAvrcpMarker::NAME, move |stream| {
            spawn_test_avrcp_client(stream, sender_test.clone());
        })
        .add_fidl_service_at(AvrcpMarker::NAME, move |stream| {
            spawn_avrcp_client(stream, sender_avrcp.clone());
        });
    fs.take_and_serve_directory_handle()?;
    fx_log_info!("Running fidl service");
    Ok(fs.collect::<()>())
}
