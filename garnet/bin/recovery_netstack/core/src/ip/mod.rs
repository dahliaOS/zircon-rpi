// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The Internet Protocol, versions 4 and 6.

mod forwarding;
mod icmp;
pub mod raw;
pub mod socket;
#[cfg(test)]
mod testdata;
mod types;

pub use self::types::*;

use log::{debug, trace};
use std::fmt::Debug;
use std::mem;

use packet::{BufferMut, BufferSerializer, ParsablePacket, ParseBufferMut, Serializer};
use zerocopy::{ByteSlice, ByteSliceMut};

use crate::address::{AddrVec, AllAddr, AutoAddr, AutoAllAddr, ConnAddr, PacketAddr};
use crate::device::{get_ip_addr, DeviceId, IpDeviceSocket};
use crate::error::NetstackError;
use crate::ip::forwarding::{Destination, ForwardingTable};
use crate::ip::raw::{RawIpEventDispatcher, RawIpState};
use crate::wire::ipv4::{Ipv4Packet, Ipv4PacketBuilder};
use crate::wire::ipv6::{Ipv6Packet, Ipv6PacketBuilder};
use crate::{Context, EventDispatcher};

// Default IPv4 TTL or IPv6 hops.
const DEFAULT_TTL: u8 = 64;

/// The state associated with the IP layer.
pub struct IpLayerState<D: EventDispatcher> {
    v4: IpLayerStateInner<Ipv4>,
    v6: IpLayerStateInner<Ipv6>,
    raw: RawIpState<D>,
}

impl<D: EventDispatcher> Default for IpLayerState<D> {
    fn default() -> IpLayerState<D> {
        IpLayerState {
            v4: IpLayerStateInner::default(),
            v6: IpLayerStateInner::default(),
            raw: RawIpState::default(),
        }
    }
}

#[derive(Default)]
struct IpLayerStateInner<I: Ip> {
    forward: bool,
    table: ForwardingTable<I>,
}

pub trait IpLayerEventDispatcher: RawIpEventDispatcher {}

#[derive(Clone)]
pub struct IpConnSocket<I: Ip> {
    addr: IpConnSocketAddr<I::Addr>,
    proto: IpProto,
    // For sockets where the socket address specifies a device, this is that
    // device. For sockets where the socket address specifies a device of "all",
    // this is kept up to date to always refer to the device which is assigned
    // the local IP.
    device: IpDeviceSocket<I>,
}

impl<I: Ip> IpConnSocket<I> {
    pub fn new<D: EventDispatcher>(
        ctx: &mut Context<D>,
        addr: IpConnSocketRequestAddr<I::Addr>,
        proto: IpProto,
    ) -> Result<IpConnSocket<I>, NetstackError> {
        let route = lookup_route(&ctx.state().ip, addr.remote_ip())?;
        let device = route.into_ip_device_socket();
        let device_addr = match addr.rest() {
            AutoAllAddr::Addr(d) => {
                if d != route.device {
                    // TODO(joshlf): What error to return here?
                    unimplemented!()
                } else {
                    AllAddr::Addr(d)
                }
            }
            AutoAllAddr::Auto => AllAddr::Addr(route.device),
            AutoAllAddr::All => AllAddr::All,
        };
        let (local_ip, _) = get_ip_addr(ctx, route.device).ok_or_else(|| unimplemented!())?;
        let local_ip = match addr.local_ip() {
            AutoAddr::Addr(l) => {
                if l != local_ip {
                    // TODO(joshlf): What error to return here?
                    unimplemented!()
                } else {
                    l
                }
            }
            AutoAddr::Auto => local_ip,
        };
        let addr = IpConnSocketAddr::new(ConnAddr::new(local_ip, addr.remote_ip()), device_addr);
        Ok(IpConnSocket { addr, proto, device })
    }

    pub fn addr(&self) -> IpConnSocketAddr<I::Addr> {
        self.addr
    }
}

#[derive(Clone)]
pub struct IpDeviceConnSocket<I: Ip> {
    addr: IpDeviceConnSocketAddr<I::Addr>,
    proto: IpProto,
}

impl<I: Ip> IpDeviceConnSocket<I> {
    pub fn new<D: EventDispatcher>(
        ctx: &mut Context<D>,
        addr: IpDeviceConnSocketRequestAddr<I::Addr>,
        proto: IpProto,
    ) -> Result<IpDeviceConnSocket<I>, NetstackError> {
        // TODO(joshlf): Verify that the named device actually exists and is up.
        // TODO(joshlf): Can loopback addresses be used?
        Ok(IpDeviceConnSocket { addr, proto })
    }

    pub fn addr(&self) -> IpDeviceConnSocketAddr<I::Addr> {
        self.addr
    }
}

#[derive(Clone)]
pub struct IpListenerSocket<I: Ip> {
    addr: IpListenerSocketAddr<I::Addr>,
    _marker: std::marker::PhantomData<I>,
}

impl<I: Ip> IpListenerSocket<I> {
    pub fn new<D: EventDispatcher>(
        ctx: &mut Context<D>,
        addr: IpListenerSocketRequestAddr<I::Addr>,
        proto: IpProto,
    ) -> Result<IpListenerSocket<I>, NetstackError> {
        unimplemented!()
    }

    pub fn addr(&self) -> IpListenerSocketAddr<I::Addr> {
        self.addr
    }
}

fn dispatch_receive_ip_packet<D: EventDispatcher, I: Ip, B: BufferMut>(
    ctx: &mut Context<D>,
    addr: IpPacketAddr<I::Addr>,
    proto: IpProto,
    mut buffer: B,
) {
    increment_counter!(ctx, "dispatch_receive_ip_packet");
    match proto {
        IpProto::Icmp | IpProto::Icmpv6 => icmp::receive_icmp_packet(ctx, addr, buffer),
        IpProto::Tcp => crate::transport::tcp::receive_ip_packet::<_, I, _>(ctx, addr, buffer),
        IpProto::Udp => crate::transport::udp::receive_ip_packet::<_, I, _>(ctx, addr, buffer),
        IpProto::Other(_) => {} // TODO(joshlf): ICMP if we don't have a handler
    }
}

/// Receive an IP packet from a device.
pub fn receive_ip_packet<D: EventDispatcher, B: BufferMut, I: Ip>(
    ctx: &mut Context<D>,
    device: DeviceId,
    mut buffer: B,
) {
    trace!("receive_ip_packet({})", device);
    let mut packet = if let Ok(packet) = buffer.parse_mut::<<I as IpExt<_>>::Packet>() {
        packet
    } else {
        // TODO(joshlf): Do something with ICMP here?
        return;
    };
    trace!("receive_ip_packet: parsed packet: {:?}", packet);

    if I::LOOPBACK_SUBNET.contains(packet.dst_ip()) {
        // A packet from outside this host was sent with the destination IP of
        // the loopback address, which is illegal. Loopback traffic is handled
        // explicitly in send_ip_packet. TODO(joshlf): Do something with ICMP
        // here?
        debug!("got packet from remote host for loopback address {}", packet.dst_ip());
    } else if deliver(ctx, device, packet.dst_ip()) {
        trace!("receive_ip_packet: delivering locally");
        // TODO(joshlf):
        // - Check for already-expired TTL?
        let addr = IpPacketAddr::<I::Addr>::from_packet(device, &packet);
        let proto = packet.proto();
        // drop packet so we can re-use the underlying buffer
        mem::drop(packet);
        dispatch_receive_ip_packet::<_, I, _>(ctx, addr, proto, buffer)
    } else if let Some(dest) = forward(ctx, packet.dst_ip()) {
        let ttl = packet.ttl();
        if ttl > 1 {
            trace!("receive_ip_packet: forwarding");
            packet.set_ttl(ttl - 1);
            let meta = packet.parse_metadata();
            // drop packet so we can re-use the underlying buffer
            mem::drop(packet);
            // Undo the effects of parsing so that the body of the buffer
            // contains the entire IP packet again (not just the body).
            buffer.undo_parse(meta);
            crate::device::send_ip_frame(
                ctx,
                &dest.into_ip_device_socket(),
                BufferSerializer::new_vec(buffer),
            );
            return;
        } else {
            // TTL is 0 or would become 0 after decrement; see "TTL" section,
            // https://tools.ietf.org/html/rfc791#page-14
            // TODO(joshlf): Do something with ICMP here?
            debug!("received IP packet dropped due to expired TTL");
        }
    } else {
        // TODO(joshlf): Do something with ICMP here?
        debug!("received IP packet with no known route to destination {}", packet.dst_ip());
    }
}

// Should we deliver this packet locally?
// deliver returns true if:
// - dst_ip is equal to the address set on the device
// - dst_ip is equal to the broadcast address of the subnet set on the device
// - dst_ip is equal to the global broadcast address
fn deliver<D: EventDispatcher, A: IpAddr>(
    ctx: &mut Context<D>,
    device: DeviceId,
    dst_ip: A,
) -> bool {
    // TODO(joshlf):
    // - This implements a strict host model (in which we only accept packets
    //   which are addressed to the device over which they were received). This
    //   is the easiest to implement for the time being, but we should actually
    //   put real thought into what our host model should be (NET-1011).
    specialize_ip_addr!(
        fn deliver(dst_ip: Self, addr_subnet: Option<(Self, Subnet<Self>)>) -> bool {
            Ipv4Addr => {
                addr_subnet
                    .map(|(addr, subnet)| dst_ip == addr || dst_ip == subnet.broadcast())
                    .unwrap_or(dst_ip == Ipv4::BROADCAST_ADDRESS)
            }
            Ipv6Addr => { log_unimplemented!(false, "ip::deliver: Ipv6 not implemeneted") }
        }
    );
    A::deliver(dst_ip, crate::device::get_ip_addr::<D, A>(ctx, device))
}

// Should we forward this packet, and if so, to whom?
fn forward<D: EventDispatcher, A: IpAddr>(
    ctx: &mut Context<D>,
    dst_ip: A,
) -> Option<Destination<A::Version>> {
    specialize_ip_addr!(
        fn forwarding_enabled<D>(state: &IpLayerState<D>) -> bool
        where
            D: EventDispatcher,
        {
            Ipv4Addr => { state.v4.forward }
            Ipv6Addr => { state.v6.forward }
        }
    );
    let ip_state = &ctx.state().ip;
    if A::forwarding_enabled(ip_state) {
        lookup_route(ip_state, dst_ip).ok()
    } else {
        None
    }
}

// Look up the route to a host.
fn lookup_route<D: EventDispatcher, A: IpAddr>(
    state: &IpLayerState<D>,
    dst_ip: A,
) -> Result<Destination<A::Version>, NetstackError> {
    specialize_ip_addr!(
        fn get_table<D>(state: &IpLayerState<D>) -> &ForwardingTable<Self::Version>
        where
            D: EventDispatcher,
        {
            Ipv4Addr => { &state.v4.table }
            Ipv6Addr => { &state.v6.table }
        }
    );
    A::get_table(state).lookup(dst_ip).ok_or_else(|| unimplemented!())
}

fn lookup_route_socket<D: EventDispatcher, A: IpAddr>(
    state: &IpLayerState<D>,
    dst_ip: A,
) -> Result<IpDeviceSocket<A::Version>, NetstackError> {
    lookup_route(state, dst_ip).map(|dest| dest.into_ip_device_socket())
}

/// Add a route to the forwarding table.
pub fn add_device_route<D: EventDispatcher, A: IpAddr>(
    ctx: &mut Context<D>,
    subnet: Subnet<A>,
    device: DeviceId,
) {
    specialize_ip_addr!(
        fn generic_add_route<D>(state: &mut IpLayerState<D>, subnet: Subnet<Self>, device: DeviceId)
        where
            D: EventDispatcher,
        {
            Ipv4Addr => { state.v4.table.add_device_route(subnet, device) }
            Ipv6Addr => { state.v6.table.add_device_route(subnet, device) }
        }
    );
    A::generic_add_route(&mut ctx.state().ip, subnet, device)
}

/// Is this one of our local addresses?
///
/// `is_local_addr` returns whether `addr` is the address associated with one of
/// our local interfaces.
pub fn is_local_addr<D: EventDispatcher, A: IpAddr>(ctx: &mut Context<D>, addr: A) -> bool {
    log_unimplemented!(false, "ip::is_local_addr: not implemented")
}

pub fn send_ip_packet<D: EventDispatcher, I, S>(
    ctx: &mut Context<D>,
    socket: &IpConnSocket<I>,
    body: S,
) where
    I: Ip,
    S: Serializer,
{
    // TODO(joshlf): Impl Display or Debug for socket types, log here
    // trace!("send_ip_packet({}, {})", dst_ip, proto);
    increment_counter!(ctx, "send_ip_packet");
    let (src_ip, dst_ip) = (socket.addr.local_ip(), socket.addr.remote_ip());
    if I::LOOPBACK_SUBNET.contains(dst_ip) {
        increment_counter!(ctx, "send_ip_packet::loopback");
        // TODO(joshlf): Currently, we serialize using the normal Serializer
        // functionality. I wonder if, in the case of delivering to loopback, we
        // can do something more efficient?
        let mut buffer = body.serialize_outer();
        // TODO(joshlf): What device should we use here?
        let addr = IpPacketAddr::new(PacketAddr::new(src_ip, dst_ip), unimplemented!());
        // TODO(joshlf): Respond with some kind of error if we don't have a
        // handler for that protocol? Maybe simulate what would have happened
        // (w.r.t ICMP) if this were a remote host?
        dispatch_receive_ip_packet::<_, I, _>(ctx, addr, socket.proto, buffer.as_buf_mut());
    } else {
        send_ip_packet_from_device(ctx, &socket.device, src_ip, dst_ip, socket.proto, body);
    }
}

pub fn send_ip_packet_from<D: EventDispatcher, I, S>(
    ctx: &mut Context<D>,
    socket: &IpDeviceConnSocket<I>,
    src_ip: I::Addr,
    body: S,
) -> Result<(), NetstackError>
where
    I: Ip,
    S: Serializer,
{
    // TODO(joshlf): Impl Display or Debug for socket types, log here
    // trace!("send_ip_packet({}, {})", dst_ip, proto);
    increment_counter!(ctx, "send_ip_packet");
    let dst_ip = socket.addr.remote_ip();
    if I::LOOPBACK_SUBNET.contains(dst_ip) {
        increment_counter!(ctx, "send_ip_packet::loopback");
        // TODO(joshlf): Currently, we serialize using the normal Serializer
        // functionality. I wonder if, in the case of delivering to loopback, we
        // can do something more efficient?
        let mut buffer = body.serialize_outer();
        // TODO(joshlf): What device should we use here?
        let addr = IpPacketAddr::new(PacketAddr::new(src_ip, dst_ip), unimplemented!());
        // TODO(joshlf): Respond with some kind of error if we don't have a
        // handler for that protocol? Maybe simulate what would have happened
        // (w.r.t ICMP) if this were a remote host?
        dispatch_receive_ip_packet::<_, I, _>(ctx, addr, socket.proto, buffer.as_buf_mut());
        Ok(())
    } else {
        let device = IpDeviceSocket::<I>::new(socket.addr.rest(), dst_ip);
        send_ip_packet_from_device(ctx, &device, src_ip, dst_ip, socket.proto, body);
        Ok(())
    }
}

fn send_ip_packet_no_socket<D: EventDispatcher, A, S>(
    ctx: &mut Context<D>,
    addr: &IpPacketAddr<A>,
    proto: IpProto,
    body: S,
) where
    A: IpAddr,
    S: Serializer,
{
    log_unimplemented!((), "ip::send_ip_packet_no_socket: Unimplemented");
}

fn send_ip_packet_from_device<D: EventDispatcher, I, S>(
    ctx: &mut Context<D>,
    socket: &IpDeviceSocket<I>,
    src_ip: I::Addr,
    dst_ip: I::Addr,
    proto: IpProto,
    body: S,
) where
    I: Ip,
    S: Serializer,
{
    assert!(!I::LOOPBACK_SUBNET.contains(src_ip));
    assert!(!I::LOOPBACK_SUBNET.contains(dst_ip));

    specialize_ip!(
        fn serialize<D, S>(
            ctx: &mut Context<D>,
            socket: &IpDeviceSocket<Self>,
            src_ip: Self::Addr,
            dst_ip: Self::Addr,
            ttl: u8,
            proto: IpProto,
            body: S
        )
        where
            D: EventDispatcher,
            S: Serializer,
        {
            Ipv4 => {
                let body = body.encapsulate(Ipv4PacketBuilder::new(src_ip, dst_ip, ttl, proto));
                crate::device::send_ip_frame(ctx, socket, body);
            }
            Ipv6 => {
                let body = body.encapsulate(Ipv6PacketBuilder::new(src_ip, dst_ip, ttl, proto));
                crate::device::send_ip_frame(ctx, socket, body);
            }
        }
    );
    I::serialize(ctx, socket, src_ip, dst_ip, DEFAULT_TTL, proto, body)
}

// An `Ip` extension trait for internal use.
//
// This trait adds extra associated types that are useful for our implementation
// here, but which consumers outside of the ip module do not need to see.
trait IpExt<B: ByteSlice>: Ip {
    type Packet: IpPacket<B, Self>;
}

impl<B: ByteSlice, I: Ip> IpExt<B> for I {
    default type Packet = !;
}

impl<B: ByteSlice> IpExt<B> for Ipv4 {
    type Packet = Ipv4Packet<B>;
}

impl<B: ByteSlice> IpExt<B> for Ipv6 {
    type Packet = Ipv6Packet<B>;
}

// `Ipv4Packet` or `Ipv6Packet`
trait IpPacket<B: ByteSlice, I: Ip>: Sized + Debug + ParsablePacket<B, ()> {
    fn src_ip(&self) -> I::Addr;
    fn dst_ip(&self) -> I::Addr;
    fn proto(&self) -> IpProto;
    fn ttl(&self) -> u8;
    fn set_ttl(&mut self, ttl: u8)
    where
        B: ByteSliceMut;
}

impl<B: ByteSlice> IpPacket<B, Ipv4> for Ipv4Packet<B> {
    fn src_ip(&self) -> Ipv4Addr {
        Ipv4Packet::src_ip(self)
    }
    fn dst_ip(&self) -> Ipv4Addr {
        Ipv4Packet::dst_ip(self)
    }
    fn proto(&self) -> IpProto {
        Ipv4Packet::proto(self)
    }
    fn ttl(&self) -> u8 {
        Ipv4Packet::ttl(self)
    }
    fn set_ttl(&mut self, ttl: u8)
    where
        B: ByteSliceMut,
    {
        Ipv4Packet::set_ttl(self, ttl)
    }
}

impl<B: ByteSlice> IpPacket<B, Ipv6> for Ipv6Packet<B> {
    fn src_ip(&self) -> Ipv6Addr {
        Ipv6Packet::src_ip(self)
    }
    fn dst_ip(&self) -> Ipv6Addr {
        Ipv6Packet::dst_ip(self)
    }
    fn proto(&self) -> IpProto {
        Ipv6Packet::proto(self)
    }
    fn ttl(&self) -> u8 {
        Ipv6Packet::hop_limit(self)
    }
    fn set_ttl(&mut self, ttl: u8)
    where
        B: ByteSliceMut,
    {
        Ipv6Packet::set_hop_limit(self, ttl)
    }
}

// This has to live in this file (as opposed to in types.rs, where all of the
// rest of these methods live) because the from_packet method needs to be
// private. If it were private and in types.rs, we wouldn't be able to use it
// from the rest of the ip module. If it were public, we would get a
// private-in-public error related to the IpExt trait.
impl<A: IpAddr> IpPacketAddr<A> {
    fn from_packet<B: ByteSlice>(
        device: DeviceId,
        packet: &<A::Version as IpExt<B>>::Packet,
    ) -> IpPacketAddr<A> {
        AddrVec::new(PacketAddr::new(packet.src_ip(), packet.dst_ip()), device)
    }
}
