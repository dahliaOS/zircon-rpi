// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    async_helpers::hanging_get::server as hanging_get,
    fidl_fuchsia_bluetooth_sys::{self as sys, AccessRequest, AccessRequestStream},
    fuchsia_async as fasync,
    fuchsia_bluetooth::types::PeerId,
    fuchsia_syslog::fx_log_warn,
    futures::{StreamExt, Stream},
    std::mem,
};

use crate::{host_dispatcher::*, watch_peers::PeerWatcher};

pub async fn run(
    hd: HostDispatcher,
    mut stream: AccessRequestStream,
) -> Result<(), Error> {
    let mut watch_peers_subscriber = hd.watch_peers().await;
    while let Some(event) = stream.next().await {
        handler(hd.clone(), &mut watch_peers_subscriber, event?).await?;
    }
    Ok(())
}

async fn handler(
    hd: HostDispatcher,
    watch_peers_subscriber: &mut hanging_get::Subscriber<PeerWatcher>,
    request: AccessRequest,
) -> Result<(), Error> {
    match request {
        AccessRequest::SetPairingDelegate{input, output, delegate, control_handle} => {
            match delegate.into_proxy() {
                Ok(proxy) => {
                        hd.set_io_capability(input, output);
                        hd.set_pairing_delegate(proxy);
                }
                Err(err) => {
                    fx_log_warn!( "Ignoring Invalid Pairing Delegate passed to SetPairingDelegate: {}", err);
                }
            }
            Ok(())
        }
        AccessRequest::SetLocalName{ name, control_handle } => {
            if let Err(e) = hd.set_name(name).await {
                fx_log_warn!("Error setting local name: {:?}", e);
            }
            Ok(())
        }
        AccessRequest::SetDeviceClass{ device_class, control_handle } => {
            if let Err(e) = hd.set_device_class(device_class).await {
                fx_log_warn!("Error setting local name: {:?}", e);
            }
            Ok(())
        }
        AccessRequest::MakeDiscoverable{ token, responder } => {
            let stream = token.into_stream().expect("The implementation of into_Stream() never fails");
            let mut result = hd.set_discoverable().await.map(|token| {
                watch_stream_for_session(stream, token);
            }).map_err(|e| e.into());
            responder.send(&mut result);
            Ok(())
        }
        AccessRequest::StartDiscovery{ token, responder } => {
            let stream = token.into_stream().expect("The implementation of into_Stream() never fails");
            let mut result = hd.start_discovery().await.map(|token| {
                watch_stream_for_session(stream, token);
            }).map_err(|e| e.into());
            responder.send(&mut result);
            Ok(())
        }
        AccessRequest::WatchPeers{ responder } => {
            match watch_peers_subscriber.register(PeerWatcher::new(responder)).await {
                Ok(()) => Ok(()),
                // TODO(nickpollard): respond with error if error?
                Err(e) => Err(format_err!("Could not watch peers")),
            }
        }
        AccessRequest::Connect{ id, responder } => {
            if let Err(e) = hd.connect(PeerId::from(id)).await {
                fx_log_warn!("Error setting local name: {:?}", e);
            }
            Ok(())
        }
        AccessRequest::Disconnect{ id, responder } => {
            if let Err(e) = hd.disconnect(PeerId::from(id)).await {
                fx_log_warn!("Error setting local name: {:?}", e);
            }
            Ok(())
        }
        AccessRequest::Forget{ id, responder } => {
            if let Err(e) = hd.forget(PeerId::from(id)).await {
                fx_log_warn!("Error setting local name: {:?}", e);
            }
            Ok(())
        }
    }
}

fn watch_stream_for_session<S: Stream + Send + 'static, T: Send + 'static>(stream: S, token: T) {
    fasync::spawn(async move {
        stream.map(|_| ()).collect::<()>().await;
        // the remote end closed; drop our session token
        mem::drop(token);
    });
}
