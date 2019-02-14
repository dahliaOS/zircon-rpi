// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The transport layer.

pub mod tcp;
pub mod udp;

use std::num::NonZeroU16;

use crate::address::{AddrVec, AllAddr, AutoAddr, ConnAddr, PacketAddress};
use crate::error::NetstackError;
use crate::ip::socket::{Bar, TransportSocket, TransportSocketImpl, TransportSocketMap};
use crate::ip::{
    Ip, IpConnSocket, IpConnSocketAddr, IpDeviceConnSocket, IpDeviceConnSocketAddr,
    IpListenerSocket, IpSocketAddr,
};
use crate::transport::tcp::TcpEventDispatcher;
use crate::transport::udp::UdpEventDispatcher;
use crate::{Context, EventDispatcher};

/// The state associated with the transport layer.
pub struct TransportLayerState<D: EventDispatcher> {
    tcp: self::tcp::TcpState<D>,
    udp: self::udp::UdpState<D>,
}

impl<D: EventDispatcher> Default for TransportLayerState<D> {
    fn default() -> TransportLayerState<D> {
        TransportLayerState {
            tcp: self::tcp::TcpState::default(),
            udp: self::udp::UdpState::default(),
        }
    }
}

/// An event dispatcher for the transport layer.
///
/// See the `EventDispatcher` trait in the crate root for more details.
pub trait TransportLayerEventDispatcher: TcpEventDispatcher + UdpEventDispatcher {}

/// The identifier for timer events in the transport layer.
#[derive(Copy, Clone, PartialEq)]
pub enum TransportLayerTimerId {
    /// A timer event in the TCP layer
    Tcp(tcp::TcpTimerId),
}

/// Handle a timer event firing in the transport layer.
pub fn handle_timeout<D: EventDispatcher>(ctx: &mut Context<D>, id: TransportLayerTimerId) {
    let TransportLayerTimerId::Tcp(id) = id;
    self::tcp::handle_timeout(ctx, id);
}

// Implementations required for the implementations of TransportSocketImpl for
// both TcpSocketImpl and UdpSocketImpl.

impl From<AllAddr<NonZeroU16>> for ConnAddr<AllAddr<NonZeroU16>, AllAddr<NonZeroU16>> {
    fn from(local: AllAddr<NonZeroU16>) -> ConnAddr<AllAddr<NonZeroU16>, AllAddr<NonZeroU16>> {
        ConnAddr::new(local, AllAddr::All)
    }
}

type ConnSocketPortPair = ConnAddr<NonZeroU16, NonZeroU16>;

type ConnSocketRequestPortPair = ConnAddr<AutoAddr<NonZeroU16>, NonZeroU16>;

/// A map of transport-layer sockets in protocols with local and remote ports.
///
/// A `PortBasedSocketMap` wraps a `TransportSocketMap` and adds functionality
/// specific to transport-layer protocols which use local and remote ports. In
/// particular, given a socket creation request with a local port of *auto*, a
/// `PortBasedSocketMap` can automatically allocate an unused local port.
struct PortBasedSocketMap<I: Ip, T: TransportSocketImpl> {
    map: TransportSocketMap<I, T>,
}

// TODO(joshlf): Redesign this algorithm. Currently, we simply randomly generate
// source ports PORT_MAP_RETRY times and then give up. This is probably good
// enough for now, but won't be in the long run.

const PORT_MAP_RETRY: usize = 256;

fn random_port_pair(remote_port: NonZeroU16) -> ConnSocketPortPair {
    use rand::Rng;
    let distr = rand::distributions::Uniform::new_inclusive(1, u16::max_value());
    let sample = rand::thread_rng().sample(distr);
    ConnAddr::new(NonZeroU16::new(sample).unwrap(), remote_port)
}

impl<I: Ip, T: TransportSocketImpl> Default for PortBasedSocketMap<I, T> {
    fn default() -> PortBasedSocketMap<I, T> {
        PortBasedSocketMap { map: TransportSocketMap::default() }
    }
}

impl<
        I: Ip,
        T: TransportSocketImpl<
            Addr = ConnAddr<AllAddr<NonZeroU16>, AllAddr<NonZeroU16>>,
            ConnAddr = ConnSocketPortPair,
            ListenerAddr = AllAddr<NonZeroU16>,
        >,
    > PortBasedSocketMap<I, T>
{
    /// Insert a new connection, allocating a local port if necessary.
    ///
    /// `insert_conn` inserts a new connection. If the local port is *auto*, it
    /// attempts to allocate an available local port automatically. On success,
    /// the chosen port pair is returned.
    fn insert_conn(
        &mut self,
        mut key: T::ConnKey,
        port_pair: ConnSocketRequestPortPair,
        mut sock: T::ConnSocket,
        mut ip_sock: IpConnSocket<I>,
    ) -> Result<ConnSocketPortPair, NetstackError> {
        if let AutoAddr::Addr(local) = port_pair.local() {
            let port_pair = ConnAddr::new(local, port_pair.remote());
            return self
                .map
                .insert_conn(key, port_pair, sock, ip_sock)
                .map(|_| port_pair)
                .map_err(|(err, _)| err);
        }

        let mut i = 0;
        loop {
            i += 1;
            let candidate_port = random_port_pair(port_pair.remote());
            match self.map.insert_conn(key, candidate_port, sock, ip_sock) {
                Ok(()) => return Ok(candidate_port),
                Err((err, (key_, _, sock_, ip_sock_))) => {
                    if i == PORT_MAP_RETRY {
                        return Err(err);
                    }
                    key = key_;
                    sock = sock_;
                    ip_sock = ip_sock_;
                }
            }
        }
    }

    /// Insert a new device connection, allocating a local port if necessary.
    ///
    /// `insert_device_conn` inserts a new device connection. If the local port
    /// is *auto*, it attempts to allocate an available local port
    /// automatically. On success, the chosen port pair is returned.
    fn insert_device_conn(
        &mut self,
        mut key: T::DeviceConnKey,
        port_pair: ConnSocketRequestPortPair,
        mut sock: T::ConnSocket,
        mut ip_sock: IpDeviceConnSocket<I>,
    ) -> Result<ConnSocketPortPair, NetstackError> {
        if let AutoAddr::Addr(local) = port_pair.local() {
            let port_pair = ConnAddr::new(local, port_pair.remote());
            return self
                .map
                .insert_device_conn(key, port_pair, sock, ip_sock)
                .map(|_| port_pair)
                .map_err(|(err, _)| err);
        }

        let mut i = 0;
        loop {
            i += 1;
            let candidate_port = random_port_pair(port_pair.remote());
            match self.map.insert_device_conn(key, candidate_port, sock, ip_sock) {
                Ok(()) => return Ok(candidate_port),
                Err((err, (key_, _, sock_, ip_sock_))) => {
                    if i == PORT_MAP_RETRY {
                        return Err(err);
                    }
                    key = key_;
                    sock = sock_;
                    ip_sock = ip_sock_;
                }
            }
        }
    }

    fn insert_listener(
        &mut self,
        key: T::ListenerKey,
        local_port: AllAddr<NonZeroU16>,
        sock: T::ListenerSocket,
        ip_sock: IpListenerSocket<I>,
    ) -> Result<(), NetstackError> {
        self.map.insert_listener(key, local_port, sock, ip_sock)
    }

    pub fn get_conn_by_key(
        &mut self,
        key: &T::ConnKey,
    ) -> Option<
        &mut TransportSocket<
            AddrVec<T::ConnAddr, IpConnSocketAddr<I::Addr>>,
            T::ConnKey,
            T::ConnSocket,
            IpConnSocket<I>,
        >,
    > {
        self.map.get_conn_by_key(key)
    }

    pub fn get_device_conn_by_key(
        &mut self,
        key: &T::DeviceConnKey,
    ) -> Option<
        &mut TransportSocket<
            AddrVec<T::ConnAddr, IpDeviceConnSocketAddr<I::Addr>>,
            T::DeviceConnKey,
            T::ConnSocket,
            IpDeviceConnSocket<I>,
        >,
    > {
        self.map.get_device_conn_by_key(key)
    }

    pub fn get_by_incoming_packet_addr<
        P: PacketAddress<SocketAddr = AddrVec<T::Addr, IpSocketAddr<I::Addr>>>,
    >(
        &mut self,
        addr: &P,
    ) -> Option<&mut Bar<I, T>> {
        self.map.get_by_incoming_packet_addr(addr)
    }
}
