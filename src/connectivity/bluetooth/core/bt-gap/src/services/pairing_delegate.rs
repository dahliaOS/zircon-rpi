// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This module defines the `PairingDispatcher`, a struct for dispatching pairing requests from
//! hosts to an upstream `PairingDelegate`, which has registered with bt-gap via the Control.fidl
//! protocol.
//!
//! A PairingDispatcher can be created via `PairingDispatcher::new()`, which returns both a
//! `PairingDispatcher` and a `PairingDispatcherHandle`.
//!
//! `PairingDispatcher::run()` returns a Future which can be polled to execute the routing
//! behavior; it will forward requests to the upstream and responses will be dispatched back to the
//! originating hosts.
//!
//! The dispatcher is communicated with via the `PairingDispatcherHandle`; the handle can be used
//! to add new hosts (via `PairingDispatcherHandle::add_host()`) and to close the pairing
//! dispatcher, which is done by simply dropping the Handle.

use {
    anyhow::Error,
    fidl_fuchsia_bluetooth_control::{
        PairingDelegateProxy,
        PairingDelegateRequest,
        PairingDelegateRequestStream,
        PairingDelegateRequest::*,
        InputCapabilityType,
        OutputCapabilityType,
    },
    fidl_fuchsia_bluetooth_host::HostProxy,
    fuchsia_async as fasync,
    fuchsia_bluetooth::{bt_fidl_status, types::PeerId},
    fuchsia_syslog::{fx_log_warn, fx_log_info},
    futures::{
        Future, TryStreamExt, select,
        future::{self, BoxFuture, FusedFuture, FutureExt, TryFutureExt},
        channel::{mpsc, oneshot},
        sink::SinkExt,
        stream::{self, StreamExt},
    },
    pin_utils::pin_mut,
    std::{
        collections::{HashMap, hash_map::Entry},
        fmt,
        task::{Poll, Context},
        pin::Pin,
    },
};

use crate::{
    host_dispatcher::HostDispatcher,
    types::HostId,
    services::pairing::{
        host_delegates::*,
        pairing_requests::*,
    },
};

type Responder = fidl_fuchsia_bluetooth_control::PairingDelegateOnPairingRequestResponder;

/// A dispatcher for routing pairing requests from hosts to an upstream delegate. See the module
/// level comment for more detail.
pub struct PairingDispatcher {
    input: InputCapabilityType,
    output: OutputCapabilityType,
    /// Host Drivers we are currently connected to
    hosts: Delegates<HostId, PairingDelegateRequestStream>,
    /// Currently in-flight requests
    inflight_requests: PairingRequests<(Result<(bool, Option<String>), fidl::Error>, Responder)>,
    /// The upstream delegate to defer requests to
    upstream: PairingDelegateProxy,
    /// New hosts that have been added
    hosts_added: mpsc::Receiver<(HostId, HostProxy)>,
    /// Notification when this has been closed
    close_triggered: oneshot::Receiver<()>,
}

impl PairingDispatcher {
    pub fn new(
        upstream_delegate: PairingDelegateProxy,
        input: InputCapabilityType,
        output: OutputCapabilityType,
    ) -> (PairingDispatcher, PairingDispatcherHandle) {
        let (hosts_sender, hosts_receiver) = mpsc::channel(0);
        let (closed_sender, closed_receiver) = oneshot::channel();

        let dispatcher = PairingDispatcher {
            input,
            output,
            hosts: Delegates::empty(),
            inflight_requests: PairingRequests::empty(),
            upstream: upstream_delegate,
            hosts_added: hosts_receiver,
            close_triggered: closed_receiver,
        };
        let handle = PairingDispatcherHandle {
            add_hosts: hosts_sender,
            close: Some(closed_sender),
        };

        (dispatcher, handle)
    }

    /// Start processing pairing requests from `host` via this upstream delegate
    fn start_handling_host(&mut self, id: HostId, host_proxy: HostProxy) -> Result<(), Error> {
        let (client, requests) = fidl::endpoints::create_request_stream()?;
        host_proxy.set_pairing_delegate(self.input, self.output, Some(client))?;
        // Traditionally we spawned a task to handle these requests
        // Instead, store a value that can be polled directly
        self.hosts.insert(id, requests);
        Ok(())
    }

    /// Once closed, no requests from the host pairers will be handled, and in-flight requests will
    /// be notified to the upstream delegate as failed.
    fn close(&mut self) {
        // Drop hosts to stop processing the tasks of all host channels
        self.hosts = Delegates::empty();
        for (_host, mut reqs) in self.inflight_requests.drain() {
            for req in reqs.drain(..) {
                let mut status = bt_fidl_status!(Failed, format!("PairingDelegate dropped, cannot complete pairing"));
                let _ignored = self.upstream.on_pairing_complete(&req.peer().to_string(), &mut status);
            }
        }
    }

    /// Run the PairingDispatcher, processing incoming host requests and responses to inflight
    /// requests from the upstream delegate
    pub async fn run(mut self) {
        loop {
            // We stop if either we're notified to stop, or the upstream delegate closes
            let upstream_closed: stream::Collect<_, ()> = self.upstream.take_event_stream().map(|_| ()).collect();
            let closed = &mut self.close_triggered;
            let mut closed = future::select(closed, upstream_closed);

            select! {
                host_event = &mut self.hosts => {
                    match host_event {
                        // A new request was received from a host
                        DelegatesMsg::Request(host, req) => self.handle_host_request(req, host),
                        // A host channel has closed; notify upstream that all of its inflight
                        // requests have aborted
                        DelegatesMsg::StreamClosed(host) => {
                            if let Some(reqs) = self.inflight_requests.remove_host(host) {
                                for req in reqs {
                                    let mut status = bt_fidl_status!(Failed, format!("Host channel dropped, cannot complete pairing"));
                                    self.upstream.on_pairing_complete(&req.peer().to_string(), &mut status);
                                }
                            }
                        }
                    }
                },
                (response, responder) = &mut self.inflight_requests => {
                    match response {
                        Ok((status, passkey)) => {
                            let result = responder.send(status, passkey.as_ref().map(String::as_str));
                            if let Err(e) = result {
                                fx_log_warn!("Error replying to pairing request from host: {}", e);
                            }
                        }
                        Err(e) if e.is_closed() => {
                            // The upstream channel has closed; we should terminate
                            self.close();
                            return;
                        }
                        Err(e) => {
                            // A non-fatal error has occurred with this particular request
                            fx_log_warn!("Error handling pairing request: {}", e);
                            let result = responder.send(false, None);
                            if let Err(e) = result {
                                fx_log_warn!("Error replying to pairing request from host: {}", e);
                            }
                        }
                    }
                },
                host_added = self.hosts_added.next().fuse() => {
                    match host_added {
                        Some((id, proxy)) => {
                            if let Err(e) = self.start_handling_host(id.clone(), proxy) {
                                fx_log_warn!("Failed to register pairing delegate channel for Host {}", id)
                            }
                        }
                        None => (),
                    }
                },
                _ = closed => {
                    self.close();
                    return;
                },
            }
        }
    }

    fn handle_host_request(&mut self, event: PairingDelegateRequest, host: HostId) {
        match event {
            OnPairingRequest { mut device, method, displayed_passkey, responder } => {
                let passkey_ref = displayed_passkey.as_ref().map(|x| &**x);
                let peer = device.identifier.parse::<PeerId>();
                match peer {
                    Ok(peer) => {
                        let response: BoxFuture<'static, _> =
                            self.upstream.on_pairing_request(&mut device, method, passkey_ref)
                            .map(move |res| { (res,responder) })
                            .boxed();
                        self.inflight_requests.insert(PairingRequest::new( host, peer, response ))
                    }
                    Err(e) => {
                        fx_log_warn!("PairingRequest received with invalid PeerId '{}': {:?}", device.identifier, e);
                        responder.send(false, None);
                    }
                }
            }
            OnPairingComplete { device_id, mut status, control_handle: _ } => {
                if self.upstream.on_pairing_complete(device_id.as_str(), &mut status).is_err() {
                    fx_log_warn!("Failed to propagate pairing cancelled upstream");
                };
            }
            OnRemoteKeypress { device_id, keypress, control_handle: _ } => {
                if self.upstream.on_remote_keypress(device_id.as_str(), keypress).is_err() {
                    fx_log_warn!("Failed to propagate pairing cancelled upstream");
                };
            }
        }
    }
}

/// A Handle to a `PairingDispatcher`. This can be used to interact with the dispatcher whilst it
/// is being executed elsewhere.
///
/// * Use `PairingDispatcherHandle::add_host()` to begin processing requests from a new Host.
/// * To terminate the PairingDispatcher, simply drop the Handle to signal to the dispatcher to
/// close. The Dispatcher termination is asynchronous - at the point the Handle is dropped, the
/// dispatcher is not guaranteed to have finished execution and must continue to be polled to
/// finalize.
pub struct PairingDispatcherHandle {
    /// Notify the PairingDispatcher to close
    close: Option<oneshot::Sender<()>>,
    /// Add a host to the PairingDispatcher
    add_hosts: mpsc::Sender<(HostId, HostProxy)>,
}

impl PairingDispatcherHandle {
    /// Add a new Host identified by `id` to this PairingDispatcher, so the dispatcher will handle
    /// pairing requests from the host serving to `proxy`.
    pub fn add_host(&mut self, id: HostId, proxy: HostProxy) {
        let mut channel = self.add_hosts.clone();
        let host_id = id.clone();
        let sent = async move {
            channel.send((host_id, proxy)).await
        };
        fasync::spawn( async move {
            if let Err(e) = sent.await {
                fx_log_info!("Failed to send channel for Host {:?} to pairing delegate", id)
            }
        });
    }
}

/// Dropping the Handle signals to the dispatcher to close. The Dispatcher termination is
/// asynchronous - at the point the Handle is dropped, the dispatcher is not guaranteed to have
/// finished execution and must continue to be polled to finalize.
impl Drop for PairingDispatcherHandle {
    fn drop(&mut self) {
        if let Some(close) = self.close.take() {
            close.send(());
        }
    }
}


// TODO(nickpollard) - test
#[cfg(test)]
mod test {

    async fn mock_delegate(PairingDelegateRequestStream) {
    }

    fn test() {
        let (upstream_delegate, delegate_server) = fidl::endpoints::create_proxy_and_stream();
        let (dispatcher, handle) = PairingDispatcher::new(upstream_delegate, InputCapabilityType::None, OutputCapabilityType::None);
        let (host_server, host_client) = fidl::endpoints::create_proxy_and_stream();

        // Task that implements the upstream delegate
        let run_upstream = mock_delegate(delegate_server);

        // Task that implements the Host
        let run_host = mock_host(host_server);

        // We can add new hosts and process their events
        handle.add_host(host_client);

        // These won't all complete here as dispatcher.run() will not complete until the handle is
        // dropped
        let (result, ) = futures::join(host_server.send(pairing_request), dispatcher.run(), run_upstream);
        assert!(result == success)

            //   // check that dropping the handle closes the delegate
            //   mem::drop(handle);
            //   dispatcher.run().await;
            //   assert!(true);
    }
}
