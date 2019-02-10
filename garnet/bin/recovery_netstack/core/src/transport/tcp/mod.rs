// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The Transmission Control Protocol (TCP).

mod conn;
mod listen;
mod types;

pub use self::types::*;

use std::hash::Hash;
use std::marker::PhantomData;
use std::num::NonZeroU16;

use packet::BufferMut;
use zerocopy::ByteSlice;

use crate::address::{AddrVec, AllAddr, ConnAddr, PacketAddr};
use crate::ip::socket::{TransportSocketImpl, TransportSocketMap};
use crate::ip::{Ip, IpAddr, IpPacketAddr, Ipv4, Ipv6};
use crate::wire::tcp::{TcpParseArgs, TcpSegment};
use crate::{Context, EventDispatcher};

/// The state associated with the TCP protocol.
pub struct TcpState<D: TcpEventDispatcher> {
    ipv4: TcpStateInner<D, Ipv4>,
    ipv6: TcpStateInner<D, Ipv6>,
}

impl<D: TcpEventDispatcher> Default for TcpState<D> {
    fn default() -> TcpState<D> {
        TcpState {
            ipv4: TcpStateInner { sockets: TransportSocketMap::default() },
            ipv6: TcpStateInner { sockets: TransportSocketMap::default() },
        }
    }
}

struct TcpStateInner<D: TcpEventDispatcher, I: Ip> {
    sockets: TransportSocketMap<I, TcpSocketImpl<D>>,
}

/// The identifier for timer events in the TCP layer.
#[derive(Copy, Clone, PartialEq)]
pub struct TcpTimerId {
    // TODO
}

// Dummy type to carry TransportSocketImpl implementation
struct TcpSocketImpl<D: TcpEventDispatcher>(PhantomData<D>);

impl<D: TcpEventDispatcher> TransportSocketImpl for TcpSocketImpl<D> {
    type Addr = ConnAddr<AllAddr<NonZeroU16>, AllAddr<NonZeroU16>>;

    type ConnKey = D::TcpConn;
    type DeviceConnKey = !;
    type ListenerKey = D::TcpListener;

    type ConnAddr = ConnAddr<NonZeroU16, NonZeroU16>;
    type DeviceConnAddr = !;
    type ListenerAddr = AllAddr<NonZeroU16>;

    type ConnSocket = ();
    type DeviceConnSocket = !;
    type ListenerSocket = ();
}

/// An event dispatcher for the TCP layer.
///
/// See the `EventDispatcher` trait in the crate root for more details.
pub trait TcpEventDispatcher {
    /// A key identifying a TCP connection.
    ///
    /// A `TcpConn` is an opaque identifier which uniquely identifies a
    /// particular TCP connection. When registering a new connection, a new
    /// `TcpConn` must be provided. When the stack invokes methods on this trait
    /// related to a connection, the corresponding `TcpConn` will be provided.
    type TcpConn: Clone + Eq + Hash;

    /// A key identifying a TCP listener.
    ///
    /// A `TcpListener` is an opaque identifier which uniquely identifies a
    /// particular TCP listener. When registering a new listener, a new
    /// `TcpListener` must be provided. When the stack invokes methods on this
    /// trait related to a listener, the corresponding `TcpListener` will be
    /// provided.
    type TcpListener: Clone + Eq + Hash;

    /// Receive data from a TCP connection.
    ///
    /// `receive_tcp_from_conn` is called when new data is available on a TCP
    /// connection for the application to consume. It consumes as much data as
    /// possible, and returns the number of bytes consumed. Whatever data is not
    /// consumed will be buffered in the TCP connection.
    fn receive_tcp_from_conn(&mut self, conn: &Self::TcpConn, data: &[u8]) -> usize {
        log_unimplemented!(0, "TcpEventDispatcher::try_receive_tcp_from_conn: not implemented")
    }
}

type TcpPacketAddr<A> = AddrVec<PacketAddr<NonZeroU16, NonZeroU16>, IpPacketAddr<A>>;

impl<A: IpAddr> TcpPacketAddr<A> {
    fn from_segment<B: ByteSlice>(
        ip: IpPacketAddr<A>,
        segment: &TcpSegment<B>,
    ) -> TcpPacketAddr<A> {
        AddrVec::new(PacketAddr::new(segment.src_port(), segment.dst_port()), ip)
    }

    fn src_ip(&self) -> A {
        self.rest().src_ip()
    }

    fn dst_ip(&self) -> A {
        self.rest().dst_ip()
    }

    fn src_port(&self) -> NonZeroU16 {
        self.head().src()
    }

    fn dst_port(&self) -> NonZeroU16 {
        self.head().dst()
    }
}

/// Receive a TCP segment in an IP packet.
pub fn receive_ip_packet<D: EventDispatcher, I: Ip, B: BufferMut>(
    ctx: &mut Context<D>,
    addr: IpPacketAddr<I::Addr>,
    mut buffer: B,
) {
    let segment = if let Ok(segment) =
        buffer.parse_with::<_, TcpSegment<_>>(TcpParseArgs::new(addr.src_ip(), addr.dst_ip()))
    {
        segment
    } else {
        // TODO(joshlf): Do something with ICMP here?
        return;
    };
}

/// Read data from a TCP connection.
///
/// `read_tcp_conn` reads up to `buf.len()` bytes from a TCP connection's local
/// buffer, and returns the number of bytes read. If fewer than `buf.len()`
/// bytes are read, then no more data is available for reading (although some
/// out-of-order data may be present in the buffer).
pub fn read_tcp_conn<D: EventDispatcher>(
    ctx: &mut Context<D>,
    conn: &D::TcpConn,
    buf: &mut [u8],
) -> usize {
    log_unimplemented!(0, "transport::tcp::read_tcp_conn: not implemented")
}

/// Handle a timer event firing in the TCP layer.
pub fn handle_timeout<D: EventDispatcher>(ctx: &mut Context<D>, id: TcpTimerId) {
    log_unimplemented!((), "transport::tcp::handle_timeout: not implemented");
}
