// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The User Datagram Protocol (UDP).

use std::hash::Hash;
use std::marker::PhantomData;
use std::num::NonZeroU16;

use packet::{BufferMut, BufferSerializer, Serializer};
use zerocopy::ByteSlice;

use crate::address::{AddrVec, AllAddr, AutoAddr, ConnAddr, PacketAddr};
use crate::error::NetstackError;
use crate::ip::socket::{Bar, TransportSocketImpl};
use crate::ip::{
    Ip, IpAddr, IpConnSocket, IpConnSocketAddr, IpConnSocketRequestAddr, IpDeviceConnSocket,
    IpDeviceConnSocketAddr, IpDeviceConnSocketRequestAddr, IpListenerSocket,
    IpListenerSocketRequestAddr, IpPacketAddr, IpProto, Ipv4, Ipv6,
};
use crate::transport::PortBasedSocketMap;
use crate::wire::udp::{UdpPacket, UdpPacketBuilder, UdpParseArgs};
use crate::{Context, EventDispatcher, StackState};

/// The state associated with the UDP protocol.
pub struct UdpState<D: UdpEventDispatcher> {
    ipv4: UdpStateInner<D, Ipv4>,
    ipv6: UdpStateInner<D, Ipv6>,
}

impl<D: UdpEventDispatcher> Default for UdpState<D> {
    fn default() -> UdpState<D> {
        UdpState {
            ipv4: UdpStateInner { sockets: PortBasedSocketMap::default() },
            ipv6: UdpStateInner { sockets: PortBasedSocketMap::default() },
        }
    }
}

struct UdpStateInner<D: UdpEventDispatcher, I: Ip> {
    sockets: PortBasedSocketMap<I, UdpSocketImpl<D>>,
}

// Dummy type to carry TransportSocketImpl implementation
struct UdpSocketImpl<D: UdpEventDispatcher>(PhantomData<D>);

impl<D: UdpEventDispatcher> TransportSocketImpl for UdpSocketImpl<D> {
    type Addr = ConnAddr<AllAddr<NonZeroU16>, AllAddr<NonZeroU16>>;

    type ConnKey = D::UdpConn;
    type DeviceConnKey = D::UdpDeviceConn;
    type ListenerKey = D::UdpListener;

    type ConnAddr = ConnAddr<NonZeroU16, NonZeroU16>;
    type ListenerAddr = AllAddr<NonZeroU16>;

    type ConnSocket = ();
    type ListenerSocket = ();
}

/// An event dispatcher for the UDP layer.
///
/// See the `EventDispatcher` trait in the crate root for more details.
pub trait UdpEventDispatcher {
    /// A key identifying a UDP connection.
    ///
    /// A `UdpConn` is an opaque identifier which uniquely identifies a
    /// particular UDP connection. When registering a new connection, a new
    /// `UdpConn` must be provided. When the stack invokes methods on this trait
    /// related to a connection, the corresponding `UdpConn` will be provided.
    type UdpConn: Clone + Eq + Hash;

    /// A key identifying a UDP all connection.
    ///
    /// A `UdpDeviceConn` is an opaque identifier which uniquely identifies a
    /// particular UDP all connection. When registering a new all connection, a
    /// new `UdpDeviceConn` must be provided. When the stack invokes methods on
    /// this trait related to an all connection, the corresponding `UdpDeviceConn`
    /// will be provided.
    type UdpDeviceConn: Clone + Eq + Hash;

    /// A key identifying a UDP listener.
    ///
    /// A `UdpListener` is an opaque identifier which uniquely identifies a
    /// particular UDP listener. When registering a new listener, a new
    /// `UdpListener` must be provided. When the stack invokes methods on this
    /// trait related to a listener, the corresponding `UdpListener` will be
    /// provided.
    type UdpListener: Clone + Eq + Hash;

    /// Receive a UDP packet for a connection.
    fn receive_udp_from_conn<A: IpAddr>(
        &mut self,
        conn: &Self::UdpConn,
        addr: UdpPacketAddr<A>,
        body: &[u8],
    ) {
        log_unimplemented!((), "UdpEventDispatcher::receive_udp_from_conn: not implemented");
    }

    fn receive_udp_from_device_conn<A: IpAddr>(
        &mut self,
        conn: &Self::UdpDeviceConn,
        addr: UdpPacketAddr<A>,
        body: &[u8],
    ) {
        log_unimplemented!((), "UdpEventDispatcher::receive_udp_from_device_conn: not implemented");
    }

    /// Receive a UDP packet for a listener.
    fn receive_udp_from_listen<A: IpAddr>(
        &mut self,
        listener: &Self::UdpListener,
        addr: UdpPacketAddr<A>,
        body: &[u8],
    ) {
        log_unimplemented!((), "UdpEventDispatcher::receive_udp_from_listen: not implemented");
    }
}

pub type UdpConnSocketAddr<A> = AddrVec<ConnAddr<NonZeroU16, NonZeroU16>, IpConnSocketAddr<A>>;

pub type UdpConnSocketRequestAddr<A> =
    AddrVec<ConnAddr<AutoAddr<NonZeroU16>, NonZeroU16>, IpConnSocketRequestAddr<A>>;

pub type UdpDeviceConnSocketAddr<A> =
    AddrVec<ConnAddr<NonZeroU16, NonZeroU16>, IpDeviceConnSocketAddr<A>>;

pub type UdpDeviceConnSocketRequestAddr<A> =
    AddrVec<ConnAddr<AutoAddr<NonZeroU16>, NonZeroU16>, IpDeviceConnSocketRequestAddr<A>>;

pub type UdpListenerSocketRequestAddr<A> =
    AddrVec<AllAddr<NonZeroU16>, IpListenerSocketRequestAddr<A>>;

type UdpPacketAddr<A> = AddrVec<PacketAddr<Option<NonZeroU16>, NonZeroU16>, IpPacketAddr<A>>;

impl<A: IpAddr> UdpPacketAddr<A> {
    fn from_packet<B: ByteSlice>(ip: IpPacketAddr<A>, packet: &UdpPacket<B>) -> UdpPacketAddr<A> {
        AddrVec::new(PacketAddr::new(packet.src_port(), packet.dst_port()), ip)
    }
}

pub fn receive_ip_packet<D: EventDispatcher, I: Ip, B: BufferMut>(
    ctx: &mut Context<D>,
    addr: IpPacketAddr<I::Addr>,
    mut buffer: B,
) {
    let packet = if let Ok(packet) =
        buffer.parse_with::<_, UdpPacket<_>>(UdpParseArgs::new(addr.src_ip(), addr.dst_ip()))
    {
        packet
    } else {
        // TODO(joshlf): Do something with ICMP here?
        return;
    };

    let (state, dispatcher) = ctx.state_and_dispatcher();
    let state = get_inner_state::<_, I>(state);

    let addr = UdpPacketAddr::from_packet(addr, &packet);
    match state.sockets.get_by_incoming_packet_addr(&addr) {
        Some(Bar::Conn(conn)) => dispatcher.receive_udp_from_conn(&conn.key, addr, packet.body()),
        Some(Bar::DeviceConn(conn)) => {
            dispatcher.receive_udp_from_device_conn(&conn.key, addr, packet.body())
        }
        Some(Bar::Listener(listener)) => {
            dispatcher.receive_udp_from_listen(&listener.key, addr, packet.body())
        }
        _ => {} // TODO(joshlf): Do something with ICMP here?
    }
}

/// Send a UDP packet on an existing connection.
///
/// # Panics
///
/// `send_udp_conn` panics if `conn` is not associated with a connection for
/// this IP version.
pub fn send_udp_conn<D: EventDispatcher, I: Ip, B: BufferMut>(
    ctx: &mut Context<D>,
    conn: &D::UdpConn,
    body: B,
) {
    let state = get_inner_state::<_, I>(ctx.state());
    let socket =
        state.sockets.get_conn_by_key(conn).expect("transport::udp::send_udp_conn: no such conn");

    let (udp, ip) = socket.addr.into_head_rest();
    let ip_sock = socket.ip_sock.clone();
    crate::ip::send_ip_packet(
        ctx,
        &ip_sock,
        BufferSerializer::new_vec(body).encapsulate(UdpPacketBuilder::new(
            ip.local_ip(),
            ip.remote_ip(),
            Some(udp.local()),
            udp.remote(),
        )),
    );
}

/// Send a UDP packet on an existing all connection.
///
/// # Panics
///
/// `send_udp_device_conn` panics if `conn` is not associated with a connection for
/// this IP version.
pub fn send_udp_device_conn<D: EventDispatcher, I: Ip, B: BufferMut>(
    ctx: &mut Context<D>,
    conn: &D::UdpDeviceConn,
    src_ip: I::Addr,
    body: B,
) -> Result<(), NetstackError> {
    let state = get_inner_state::<_, I>(ctx.state());
    let socket = state
        .sockets
        .get_device_conn_by_key(conn)
        .expect("transport::udp::send_udp_conn: no such conn");

    let (udp, remote_ip) = socket.addr.into_head_rest();
    let ip_sock = socket.ip_sock.clone();
    crate::ip::send_ip_packet_from(
        ctx,
        &ip_sock,
        src_ip,
        BufferSerializer::new_vec(body).encapsulate(UdpPacketBuilder::new(
            src_ip,
            remote_ip.head(),
            Some(udp.local()),
            udp.remote(),
        )),
    )
}

pub fn connect_udp<D: EventDispatcher, I: Ip>(
    ctx: &mut Context<D>,
    conn: D::UdpConn,
    addr: UdpConnSocketRequestAddr<I::Addr>,
) -> Result<(), NetstackError> {
    let (udp, ip) = addr.into_head_rest();
    let socket = IpConnSocket::new(ctx, ip, IpProto::Udp)?;
    let state = get_inner_state::<_, I>(ctx.state());
    state.sockets.insert_conn(conn, udp, (), socket).map(|_| ())
}

pub fn connect_all_udp<D: EventDispatcher, I: Ip>(
    ctx: &mut Context<D>,
    device_conn: D::UdpDeviceConn,
    addr: UdpDeviceConnSocketRequestAddr<I::Addr>,
) -> Result<(), NetstackError> {
    let (udp, ip) = addr.into_head_rest();
    let socket = IpDeviceConnSocket::new(ctx, ip, IpProto::Udp)?;
    let state = get_inner_state::<_, I>(ctx.state());
    state.sockets.insert_device_conn(device_conn, udp, (), socket).map(|_| ())
}

pub fn listen_udp<D: EventDispatcher, I: Ip>(
    ctx: &mut Context<D>,
    listener: D::UdpListener,
    addr: UdpListenerSocketRequestAddr<I::Addr>,
) -> Result<(), NetstackError> {
    let (udp, ip) = addr.into_head_rest();
    let socket = IpListenerSocket::new(ctx, ip, IpProto::Udp)?;
    let state = get_inner_state::<_, I>(ctx.state());
    state.sockets.insert_listener(listener, udp, (), socket).map(|_| ())
}

fn get_inner_state<D: EventDispatcher, I: Ip>(
    state: &mut StackState<D>,
) -> &mut UdpStateInner<D, I> {
    specialize_ip!(
        fn get_inner_state<D>(state: &mut UdpState<D>) -> &mut UdpStateInner<D, Self>
        where
            D: EventDispatcher,
        {
            Ipv4 => { &mut state.ipv4 }
            Ipv6 => { &mut state.ipv6 }
        }
    );
    I::get_inner_state(&mut state.transport.udp)
}
