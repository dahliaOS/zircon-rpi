// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::host_dispatcher::*;
use failure::Error;
use fidl::endpoints::RequestStream;
use fidl_fuchsia_bluetooth_control::{ControlRequest, ControlRequestStream};
use fuchsia_async as fasync;
use fuchsia_bluetooth::bt_fidl_status;
use futures::prelude::*;

use crate::actor::{self, ask, ActorHandle};
use crate::discovery::*;
use crate::host::Host;
use crate::types::bt;

struct ControlSession {
    discovery_session: Option<DiscoverySession>,
}

impl ControlSession {
    fn new() -> ControlSession {
        ControlSession { discovery_session: None }
    }
}

pub async fn control_service<H: Host>(system: actor::System, d: ActorHandle<HostDispatcherMsg<H>>, chan: fasync::Channel) -> Result<(), Error> {
    let mut stream = ControlRequestStream::from_channel(chan);
    let mut session = ControlSession::new();

    //let event_listener = Arc::new(stream.control_handle());
    //hd.add_event_listener(Arc::downgrade(&event_listener));

    while let Some(event) = await!(stream.next()) {
        await!(handler(&mut session, system.clone(), d.clone(), event?))?;
    }
    Ok(())
}

async fn handler<H: Host>(session: &mut ControlSession, mut system: actor::System, mut d: ActorHandle<HostDispatcherMsg<H>>, event: ControlRequest) -> fidl::Result<()> {
    match event {
        ControlRequest::RequestDiscovery { discovery, responder } => {
            if discovery {
                let result = await!(ask(&mut system, &mut d, |h| HostDispatcherMsg::StartDiscovery(h)));
                match result {
                    Ok(result) => match result {
                        Ok(discovery) => {
                            session.discovery_session = Some(discovery);
                            let _ignored = responder.send(&mut bt_fidl_status!());
                        }
                        Err(_err) => {
                            // Actor response failure
                            let _ignored = responder.send(&mut bt_fidl_status!(Failed, "No Adapters to start discovery"));
                        }
                    },
                    Err(_err) => {
                        let _ignored = responder.send(&mut bt_fidl_status!(BluetoothNotAvailable, "No Adapters to start discovery"));
                    }
                }
            } else {
                let _dropped = session.discovery_session.take();
            }
            Ok(())
        }
        _ => Ok(())
    }
}
