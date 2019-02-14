// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::hash::Hash;
use std::marker::PhantomData;

use packet::{BufferMut, BufferSerializer};

use crate::address::{AddrVec, AllAddr};
use crate::error::NetstackError;
use crate::ip::socket::{Bar, TransportSocketImpl, TransportSocketMap};
use crate::ip::{
    Ip, IpAddr, IpConnSocket, IpConnSocketAddr, IpConnSocketRequestAddr, IpDeviceConnSocket,
    IpDeviceConnSocketAddr, IpDeviceConnSocketRequestAddr, IpListenerSocketRequestAddr,
    IpPacketAddr, IpProto, Ipv4, Ipv6,
};
use crate::{Context, EventDispatcher, StackState};

pub struct RawIpState<D: RawIpEventDispatcher> {
    ipv4: RawIpStateInner<D, Ipv4>,
    ipv6: RawIpStateInner<D, Ipv6>,
}

impl<D: RawIpEventDispatcher> Default for RawIpState<D> {
    fn default() -> RawIpState<D> {
        RawIpState {
            ipv4: RawIpStateInner { sockets: TransportSocketMap::default() },
            ipv6: RawIpStateInner { sockets: TransportSocketMap::default() },
        }
    }
}

struct RawIpStateInner<D: RawIpEventDispatcher, I: Ip> {
    sockets: TransportSocketMap<I, IpSocketImpl<D>>,
}

// Dummy type to carry TransportSocketImpl implementation
struct IpSocketImpl<D: RawIpEventDispatcher>(PhantomData<D>);

impl<D: RawIpEventDispatcher> TransportSocketImpl for IpSocketImpl<D> {
    type Addr = AllAddr<IpProto>;

    type ConnKey = D::IpConn;
    type DeviceConnKey = D::IpDeviceConn;
    type ListenerKey = D::IpListener;

    type ConnAddr = IpProto;
    type ListenerAddr = IpProto;

    type ConnSocket = ();
    type ListenerSocket = ();
}

/// An event dispatcher for raw IP sockets.
///
/// See the `EventDispatcher` trait in the crate root for more details.
pub trait RawIpEventDispatcher {
    /// A key identifying an IP connection.
    ///
    /// A `IpConn` is an opaque identifier which uniquely identifies a
    /// particular IP connection. When registering a new connection, a new
    /// `IpConn` must be provided. When the stack invokes methods on this trait
    /// related to a connection, the corresponding `IpConn` will be provided.
    type IpConn: Clone + Eq + Hash;

    /// A key identifying an IP all connection.
    ///
    /// A `IpDeviceConn` is an opaque identifier which uniquely identifies a
    /// particular IP all connection. When registering a new all connection, a
    /// new `IpDeviceConn` must be provided. When the stack invokes methods on this
    /// trait related to an all connection, the corresponding `IpDeviceConn` will
    /// be provided.
    type IpDeviceConn: Clone + Eq + Hash;

    /// A key identifying an IP listener.
    ///
    /// An `IpListener` is an opaque identifier which uniquely identifies a
    /// particular IP listener. When registering a new listener, a new
    /// `IpListener` must be provided. When the stack invokes methods on this
    /// trait related to a listener, the corresponding `IpListener` will be
    /// provided.
    type IpListener: Clone + Eq + Hash;

    /// Receive an IP packet for a connection.
    fn receive_ip_from_conn<A: IpAddr>(
        &mut self,
        conn: &Self::IpConn,
        addr: RawIpPacketAddr<A>,
        body: &[u8],
    ) {
        log_unimplemented!((), "RawIpEventDispatcher::receive_ip_from_conn: not implemented");
    }

    /// Receive an IP packet for an all connection.
    fn receive_ip_from_device_conn<A: IpAddr>(
        &mut self,
        conn: &Self::IpDeviceConn,
        addr: RawIpPacketAddr<A>,
        body: &[u8],
    ) {
        log_unimplemented!(
            (),
            "RawIpEventDispatcher::receive_ip_from_device_conn: not implemented"
        );
    }

    /// Receive an IP packet for a listener.
    fn receive_ip_from_listen<A: IpAddr>(
        &mut self,
        listener: &Self::IpListener,
        addr: RawIpPacketAddr<A>,
        body: &[u8],
    ) {
        log_unimplemented!((), "RawIpEventDispatcher::receive_ip_from_listen: not implemented");
    }
}

pub type RawIpConnSocketAddr<A> = AddrVec<IpProto, IpConnSocketAddr<A>>;

pub type RawIpConnSocketRequestAddr<A> = AddrVec<IpProto, IpConnSocketRequestAddr<A>>;

pub type RawIpDeviceConnSocketAddr<A> = AddrVec<IpProto, IpDeviceConnSocketAddr<A>>;

pub type RawIpDeviceConnSocketRequestAddr<A> = AddrVec<IpProto, IpDeviceConnSocketRequestAddr<A>>;

pub type RawIpListenerSocketRequestAddr<A> = AddrVec<IpProto, IpListenerSocketRequestAddr<A>>;

type RawIpPacketAddr<A> = AddrVec<IpProto, IpPacketAddr<A>>;

pub fn receive_ip_packet<D: EventDispatcher, I: Ip>(
    ctx: &mut Context<D>,
    addr: IpPacketAddr<I::Addr>,
    proto: IpProto,
    body: &[u8],
) {
    let (state, dispatcher) = ctx.state_and_dispatcher();
    let state = get_inner_state::<_, I>(state);

    let addr = AddrVec::new(proto, addr);
    match state.sockets.get_by_incoming_packet_addr(&addr) {
        Some(Bar::Conn(conn)) => dispatcher.receive_ip_from_conn(&conn.key, addr, body),
        Some(Bar::DeviceConn(conn)) => {
            dispatcher.receive_ip_from_device_conn(&conn.key, addr, body)
        }
        Some(Bar::Listener(listener)) => {
            dispatcher.receive_ip_from_listen(&listener.key, addr, body)
        }
        _ => {} // TODO(joshlf): Do something with ICMP here?
    }
}

/// Send an IP packet on an existing connection.
///
/// # Panics
///
/// `send_ip_conn` panics if `conn` is not associated with a connection for this
/// IP version.
pub fn send_ip_conn<D: EventDispatcher, I: Ip, B: BufferMut>(
    ctx: &mut Context<D>,
    conn: &D::IpConn,
    body: B,
) {
    let state = get_inner_state::<_, I>(ctx.state());
    let socket = state.sockets.get_conn_by_key(conn).expect("ip::raw::send_ip_conn: no such conn");

    let ip_sock = socket.ip_sock.clone();
    crate::ip::send_ip_packet(ctx, &ip_sock, BufferSerializer::new_vec(body));
}

/// Send an IP packet on an existing all connection.
///
/// # Panics
///
/// `send_ip_device_conn` panics if `conn` is not associated with a connection for
/// this IP version.
pub fn send_ip_device_conn<D: EventDispatcher, I: Ip, B: BufferMut>(
    ctx: &mut Context<D>,
    conn: &D::IpDeviceConn,
    src_ip: I::Addr,
    body: B,
) -> Result<(), NetstackError> {
    let state = get_inner_state::<_, I>(ctx.state());
    let socket =
        state.sockets.get_device_conn_by_key(conn).expect("ip::raw::send_ip_conn: no such conn");

    let ip_sock = socket.ip_sock.clone();
    crate::ip::send_ip_packet_from(ctx, &ip_sock, src_ip, BufferSerializer::new_vec(body))
}

pub fn connect_ip<D: EventDispatcher, I: Ip>(
    ctx: &mut Context<D>,
    conn: D::IpConn,
    addr: RawIpConnSocketRequestAddr<I::Addr>,
) -> Result<(), NetstackError> {
    let (proto, ip) = addr.into_head_rest();
    let socket = IpConnSocket::new(ctx, ip, proto)?;
    let state = get_inner_state::<_, I>(ctx.state());
    state.sockets.insert_conn(conn, proto, (), socket).map(|_| ()).map_err(|(err, _)| err)
}

pub fn connect_all_ip<D: EventDispatcher, I: Ip>(
    ctx: &mut Context<D>,
    device_conn: D::IpDeviceConn,
    addr: RawIpDeviceConnSocketRequestAddr<I::Addr>,
) -> Result<(), NetstackError> {
    let (proto, ip) = addr.into_head_rest();
    let socket = IpDeviceConnSocket::new(ctx, ip, proto)?;
    let state = get_inner_state::<_, I>(ctx.state());
    state
        .sockets
        .insert_device_conn(device_conn, proto, (), socket)
        .map(|_| ())
        .map_err(|(err, _)| err)
}

pub fn listen_ip<D: EventDispatcher, I: Ip>(
    ctx: &mut Context<D>,
    listener: D::IpListener,
    addr: RawIpListenerSocketRequestAddr<I::Addr>,
) -> Result<(), NetstackError> {
    unimplemented!()
}

fn get_inner_state<D: EventDispatcher, I: Ip>(
    state: &mut StackState<D>,
) -> &mut RawIpStateInner<D, I> {
    specialize_ip!(
        fn get_inner_state<D>(state: &mut RawIpState<D>) -> &mut RawIpStateInner<D, Self>
        where
            D: EventDispatcher,
        {
            Ipv4 => { &mut state.ipv4 }
            Ipv6 => { &mut state.ipv6 }
        }
    );
    I::get_inner_state(&mut state.ip.raw)
}
