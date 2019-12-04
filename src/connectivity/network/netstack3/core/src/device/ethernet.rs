// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The Ethernet protocol.

use std::collections::HashMap;
use std::fmt::Debug;

use log::{debug, trace};
use net_types::ethernet::Mac;
#[cfg(test)]
use net_types::ip::{AddrSubnet, Ipv6Addr};
use net_types::ip::{Ip, IpVersion, Ipv4, Ipv4Addr, Ipv6};
use net_types::{BroadcastAddress, MulticastAddr, MulticastAddress, UnicastAddress, Witness};
use packet::{BufferMut, Nested, Serializer};

use crate::context::{FrameContext, InstantContext, StateContext};
use crate::device::arp::{self, ArpFrameMetadata, ArpHardwareType, ArpState};
use crate::device::iplink::{
    BufferIpLinkDeviceContext, BufferIpLinkDeviceHandler, IpLinkDeviceContext,
    IpLinkDeviceIdContext, IpLinkDeviceTimerId,
};
use crate::device::link::{
    LinkDevice, LinkDeviceContext, LinkDeviceContextImpl, LinkDeviceHandler, LinkDeviceIdContext,
    LinkFrameMeta,
};
use crate::device::ndp::{self, NdpState};
#[cfg(test)]
use crate::device::AddressError;
use crate::device::{FrameDestination, IpLinkDeviceState, RecvIpFrameMeta};
use crate::wire::arp::peek_arp_types;
use crate::wire::ethernet::{EthernetFrame, EthernetFrameBuilder};
use crate::Instant;
use crate::{BufferDispatcher, Context};

/// Device IDs identifying Ethernet devices.
#[derive(Copy, Clone, Eq, PartialEq, Hash)]
pub(super) struct EthernetDeviceId(pub(super) usize);

impl From<usize> for EthernetDeviceId {
    fn from(id: usize) -> EthernetDeviceId {
        EthernetDeviceId(id)
    }
}

impl From<Mac> for FrameDestination {
    fn from(mac: Mac) -> FrameDestination {
        if mac.is_broadcast() {
            FrameDestination::Broadcast
        } else if mac.is_multicast() {
            FrameDestination::Multicast
        } else {
            debug_assert!(mac.is_unicast());
            FrameDestination::Unicast
        }
    }
}

create_protocol_enum!(
    /// An EtherType number.
    #[derive(Copy, Clone, Hash, Eq, PartialEq)]
    pub(crate) enum EtherType: u16 {
        Ipv4, 0x0800, "IPv4";
        Arp, 0x0806, "ARP";
        Ipv6, 0x86DD, "IPv6";
        _, "EtherType {}";
    }
);

impl From<IpVersion> for EtherType {
    fn from(v: IpVersion) -> EtherType {
        match v {
            IpVersion::V4 => EtherType::Ipv4,
            IpVersion::V6 => EtherType::Ipv6,
        }
    }
}

impl<C: LinkDeviceIdContext<EthernetLinkDevice> + InstantContext>
    LinkDeviceContextImpl<EthernetLinkDevice> for C
{
    type State = EthernetDeviceState<<C as InstantContext>::Instant>;
}

pub(super) trait EthernetLinkDeviceContextImpl:
    InstantContext
    + LinkDeviceIdContext<EthernetLinkDevice>
    + LinkDeviceContextImpl<
        EthernetLinkDevice,
        State = EthernetDeviceState<<Self as InstantContext>::Instant>,
    >
{
}
impl<
        C: InstantContext
            + LinkDeviceIdContext<EthernetLinkDevice>
            + LinkDeviceContextImpl<
                EthernetLinkDevice,
                State = EthernetDeviceState<<C as InstantContext>::Instant>,
            >,
    > EthernetLinkDeviceContextImpl for C
{
}

pub(super) trait EthernetLinkDeviceContext:
    EthernetLinkDeviceContextImpl + LinkDeviceContext<EthernetLinkDevice>
{
}
impl<C: EthernetLinkDeviceContextImpl + LinkDeviceContext<EthernetLinkDevice>>
    EthernetLinkDeviceContext for C
{
}

/// A shorthand for `IpLinkDeviceContext` with all of the appropriate type arguments
/// fixed to their Ethernet values.
pub(super) trait EthernetIpLinkDeviceContext:
    EthernetLinkDeviceContext
    + IpLinkDeviceContext<Ipv4, EthernetLinkDevice>
    + IpLinkDeviceContext<Ipv6, EthernetLinkDevice>
{
}
impl<
        C: EthernetLinkDeviceContext
            + IpLinkDeviceContext<Ipv4, EthernetLinkDevice>
            + IpLinkDeviceContext<Ipv6, EthernetLinkDevice>,
    > EthernetIpLinkDeviceContext for C
{
}

/// A shorthand for `BufferIpLinkDeviceContext` with all of the appropriate type
/// arguments fixed to their Ethernet values.
pub(super) trait BufferEthernetIpLinkDeviceContext<B: BufferMut>:
    EthernetIpLinkDeviceContext
    + BufferIpLinkDeviceContext<Ipv4, EthernetLinkDevice, B>
    + BufferIpLinkDeviceContext<Ipv6, EthernetLinkDevice, B>
{
}

impl<
        B: BufferMut,
        C: EthernetIpLinkDeviceContext
            + BufferIpLinkDeviceContext<Ipv4, EthernetLinkDevice, B>
            + BufferIpLinkDeviceContext<Ipv6, EthernetLinkDevice, B>,
    > BufferEthernetIpLinkDeviceContext<B> for C
{
}

/// A shorthand for `BufferIpLinkDeviceContext` with all of the appropriate type
/// arguments fixed to their Ethernet values.
pub(super) trait BufferEthernetIpLinkDeviceHandler<I: Ip, B: BufferMut>:
    BufferIpLinkDeviceHandler<I, EthernetLinkDevice, B>
{
}

impl<I: Ip, B: BufferMut, C: BufferIpLinkDeviceHandler<I, EthernetLinkDevice, B>>
    BufferEthernetIpLinkDeviceHandler<I, B> for C
{
}

/// Builder for [`EthernetDeviceState`].
pub(crate) struct EthernetDeviceStateBuilder {
    mac: Mac,
    mtu: u32,
    ndp_configs: ndp::NdpConfigurations,
}

impl EthernetDeviceStateBuilder {
    /// Create a new `EthernetDeviceStateBuilder`.
    pub(crate) fn new(mac: Mac, mtu: u32) -> Self {
        // TODO(joshlf): Add a minimum MTU for all Ethernet devices such that
        //  you cannot create an `EthernetDeviceState` with an MTU smaller than
        //  the minimum. The absolute minimum needs to be at least the minimum
        //  body size of an Ethernet frame. For IPv6-capable devices, the
        //  minimum needs to be higher - the IPv6 minimum MTU. The easy path is
        //  to simply use the IPv6 minimum MTU as the minimum in all cases,
        //  although we may at some point want to figure out how to configure
        //  devices which don't support IPv6, and allow smaller MTUs for those
        //  devices.
        //  A few questions:
        //  - How do we wire error information back up the call stack? Should
        //  this just return a Result or something?

        Self { mac, mtu, ndp_configs: ndp::NdpConfigurations::default() }
    }

    /// Update the NDP configurations that will be set on the ethernet
    /// device.
    pub(crate) fn set_ndp_configs(&mut self, v: ndp::NdpConfigurations) {
        self.ndp_configs = v;
    }

    /// Build the `EthernetDeviceState` from this builder.
    pub(super) fn build<I: Instant>(self) -> EthernetDeviceState<I> {
        EthernetDeviceState {
            mac: self.mac,
            mtu: self.mtu,
            hw_mtu: self.mtu,
            link_multicast_groups: HashMap::new(),
            ipv4_arp: ArpState::default(),
            ndp: NdpState::new(self.ndp_configs),
            promiscuous_mode: false,
        }
    }
}

/// The state associated with an Ethernet device.
pub(super) struct EthernetDeviceState<I: Instant> {
    /// Mac address of the device this state is for.
    pub(super) mac: Mac,

    /// The value this netstack assumes as the device's current MTU.
    pub(super) mtu: u32,

    /// The maximum MTU allowed by the hardware.
    ///
    /// `mtu` MUST NEVER be greater than `hw_mtu`.
    pub(super) hw_mtu: u32,

    /// Link multicast groups this device has joined.
    link_multicast_groups: HashMap<MulticastAddr<Mac>, usize>,

    /// IPv4 ARP state.
    ipv4_arp: ArpState<EthernetLinkDevice, Ipv4Addr>,

    /// (IPv6) NDP state.
    ndp: ndp::NdpState<EthernetLinkDevice, I>,

    /// A flag indicating whether the device will accept all ethernet frames that it receives,
    /// regardless of the ethernet frame's destination MAC address.
    promiscuous_mode: bool,
}

impl<I: Instant> EthernetDeviceState<I> {
    /// Is a packet with a destination MAC address, `dst`, destined for this device?
    ///
    /// Returns `true` if this device is has `dst_mac` as its assigned MAC address, `dst_mac` is the
    /// broadcast MAC address, or it is one of the multicast MAC addresses the device has joined.
    fn should_accept(&self, dst_mac: &Mac) -> bool {
        (self.mac == *dst_mac)
            || dst_mac.is_broadcast()
            || (MulticastAddr::new(*dst_mac)
                .map(|a| self.link_multicast_groups.contains_key(&a))
                .unwrap_or(false))
    }

    /// Should a packet with destination MAC address, `dst`, be accepted by this device?
    ///
    /// Returns `true` if this device is in promiscuous mode or the frame is destined for this
    /// device.
    fn should_deliver(&self, dst_mac: &Mac) -> bool {
        self.promiscuous_mode || self.should_accept(dst_mac)
    }
}

/// An extension trait adding IP-related functionality to `Ipv4` and `Ipv6`.
pub(crate) trait EthernetIpExt: Ip {
    const ETHER_TYPE: EtherType;
}

impl<I: Ip> EthernetIpExt for I {
    default const ETHER_TYPE: EtherType = EtherType::Ipv4;
}

impl EthernetIpExt for Ipv4 {
    const ETHER_TYPE: EtherType = EtherType::Ipv4;
}

impl EthernetIpExt for Ipv6 {
    const ETHER_TYPE: EtherType = EtherType::Ipv6;
}

/// A timer ID for Ethernet devices.
///
/// `D` is the type of device ID that identifies different Ethernet devices.
pub(super) type EthernetTimerId<D> = IpLinkDeviceTimerId<EthernetLinkDevice, D>;

impl<B: BufferMut, D: BufferDispatcher<B>>
    FrameContext<B, LinkFrameMeta<EthernetDeviceId, Mac, EtherType>> for Context<D>
{
    fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        meta: LinkFrameMeta<EthernetDeviceId, Mac, EtherType>,
        body: S,
    ) -> Result<(), S> {
        let state = <Context<D> as StateContext<IpLinkDeviceState<_, _>, _>>::get_state_with(
            self,
            meta.device,
        )
        .link();
        let (local_mac, mtu) = (state.mac, state.mtu);

        self.send_frame(
            meta.device,
            body.with_mtu(mtu as usize).encapsulate(EthernetFrameBuilder::new(
                local_mac,
                meta.dst_addr,
                meta.link_specific,
            )),
        )
        .map_err(|ser| ser.into_inner().into_inner())
    }
}

/// Receive an Ethernet frame from the network.
pub(super) fn receive_frame<B: BufferMut, C: BufferEthernetIpLinkDeviceContext<B>>(
    ctx: &mut C,
    device_id: <C as LinkDeviceIdContext<EthernetLinkDevice>>::DeviceId,
    mut buffer: B,
) {
    trace!("ethernet::receive_frame: device_id = {:?}", device_id);
    let frame = if let Ok(frame) = buffer.parse::<EthernetFrame<_>>() {
        frame
    } else {
        trace!("ethernet::receive_frame: failed to parse ethernet frame");
        // TODO(joshlf): Do something else?
        return;
    };

    let (_, dst) = (frame.src_mac(), frame.dst_mac());

    if !<C as StateContext<IpLinkDeviceState<_, _>, _>>::get_state_with(ctx, device_id)
        .link()
        .should_deliver(&dst)
    {
        trace!("ethernet::receive_frame: destination mac {:?} not for device {:?}", dst, device_id);
        return;
    }

    let frame_dst = FrameDestination::from(dst);

    match frame.ethertype() {
        Some(EtherType::Arp) => {
            let types = if let Ok(types) = peek_arp_types(buffer.as_ref()) {
                types
            } else {
                // TODO(joshlf): Do something else here?
                return;
            };
            match types {
                (ArpHardwareType::Ethernet, EtherType::Ipv4) => {
                    arp::receive_arp_packet(ctx, device_id, buffer)
                }
                types => debug!("got ARP packet for unsupported types: {:?}", types),
            }
        }
        Some(EtherType::Ipv4) => {
            ctx.receive_frame(RecvIpFrameMeta::<_, Ipv4>::new(device_id, frame_dst), buffer)
        }
        Some(EtherType::Ipv6) => {
            ctx.receive_frame(RecvIpFrameMeta::<_, Ipv6>::new(device_id, frame_dst), buffer)
        }
        Some(EtherType::Other(_)) | None => {} // TODO(joshlf)
    }
}

/// Set the promiscuous mode flag on `device_id`.
pub(super) fn set_promiscuous_mode<C: EthernetLinkDeviceContext>(
    ctx: &mut C,
    device_id: <C as LinkDeviceIdContext<EthernetLinkDevice>>::DeviceId,
    enabled: bool,
) {
    <C as StateContext<IpLinkDeviceState<_, _>, _>>::get_state_mut_with(ctx, device_id)
        .link_mut()
        .promiscuous_mode = enabled;
}

/// Add `device_id` to a link multicast group `multicast_addr`.
///
/// Calling `join_link_multicast` with the same `device_id` and `multicast_addr` is completely safe.
/// A counter will be kept for the number of times `join_link_multicast` has been called with the
/// same `device_id` and `multicast_addr` pair. To completely leave a multicast group,
/// [`leave_link_multicast`] must be called the same number of times `join_link_multicast` has been
/// called for the same `device_id` and `multicast_addr` pair. The first time `join_link_multicast`
/// is called for a new `device` and `multicast_addr` pair, the device will actually join the
/// multicast group.
///
/// `join_link_multicast` is different from [`join_ip_multicast`] as `join_link_multicast` joins an
/// L2 multicast group, whereas `join_ip_multicast` joins an L3 multicast group.
pub(super) fn join_link_multicast<C: EthernetLinkDeviceContext>(
    ctx: &mut C,
    device_id: <C as LinkDeviceIdContext<EthernetLinkDevice>>::DeviceId,
    multicast_addr: MulticastAddr<Mac>,
) {
    let device_state =
        <C as StateContext<IpLinkDeviceState<_, _>, _>>::get_state_mut_with(ctx, device_id)
            .link_mut();

    let groups = &mut device_state.link_multicast_groups;

    let counter = groups.entry(multicast_addr).or_insert(0);
    *counter += 1;

    if *counter == 1 {
        trace!("ethernet::join_link_multicast: joining link multicast {:?}", multicast_addr,);
    } else {
        trace!(
            "ethernet::join_link_multicast: already joinined link multicast {:?}, counter = {}",
            multicast_addr,
            *counter,
        );
    }
}

/// Remove `device_id` from a link multicast group `multicast_addr`.
///
/// `leave_link_multicast` will attempt to remove `device_id` from the multicast group
/// `multicast_addr`. `device_id` may have "joined" the same multicast address multiple times, so
/// `device_id` will only leave the multicast group once `leave_ip_multicast` has been called for
/// each corresponding [`join_link_multicast`]. That is, if `join_link_multicast` gets called 3
/// times and `leave_link_multicast` gets called two times (after all 3 `join_link_multicast`
/// calls), `device_id` will still be in the multicast group until the next (final) call to
/// `leave_link_multicast`.
///
/// `leave_link_multicast` is different from [`leave_ip_multicast`] as `leave_link_multicast` leaves
/// an L2 multicast group, whereas `leave_ip_multicast` leaves an L3 multicast group.
///
/// # Panics
///
/// If `device_id` is not in the multicast group `multicast_addr`.
pub(super) fn leave_link_multicast<C: EthernetLinkDeviceContext>(
    ctx: &mut C,
    device_id: <C as LinkDeviceIdContext<EthernetLinkDevice>>::DeviceId,
    multicast_addr: MulticastAddr<Mac>,
) {
    let device_state =
        <C as StateContext<IpLinkDeviceState<_, _>, _>>::get_state_mut_with(ctx, device_id)
            .link_mut();

    let groups = &mut device_state.link_multicast_groups;

    // Will panic if `device_id` has not yet joined the multicast address.
    let counter = groups.get_mut(&multicast_addr).unwrap();

    if *counter == 1 {
        trace!("ethernet::leave_link_multicast: leaving link multicast {:?}", multicast_addr,);

        groups.remove(&multicast_addr);
    } else {
        *counter -= 1;

        trace!(
            "ethernet::leave_link_multicast: not leaving link multicast {:?} as there are still listeners for it, counter = {}",
            multicast_addr,
            *counter,
        );
    }
}

impl<
        Id,
        C: InstantContext
            + StateContext<
                IpLinkDeviceState<
                    <C as InstantContext>::Instant,
                    EthernetDeviceState<<C as InstantContext>::Instant>,
                >,
                Id,
            >,
    > StateContext<ArpState<EthernetLinkDevice, Ipv4Addr>, Id> for C
{
    fn get_state_with(&self, id: Id) -> &ArpState<EthernetLinkDevice, Ipv4Addr> {
        &self.get_state_with(id).link().ipv4_arp
    }

    fn get_state_mut_with(&mut self, id: Id) -> &mut ArpState<EthernetLinkDevice, Ipv4Addr> {
        &mut self.get_state_mut_with(id).link_mut().ipv4_arp
    }
}

impl<
        C: IpLinkDeviceIdContext<EthernetLinkDevice>
            + LinkDeviceHandler<EthernetLinkDevice>
            + FrameContext<B, <C as LinkDeviceIdContext<EthernetLinkDevice>>::DeviceId>,
        B: BufferMut,
    >
    FrameContext<
        B,
        ArpFrameMetadata<
            EthernetLinkDevice,
            <C as LinkDeviceIdContext<EthernetLinkDevice>>::DeviceId,
        >,
    > for C
{
    fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        meta: ArpFrameMetadata<
            EthernetLinkDevice,
            <C as LinkDeviceIdContext<EthernetLinkDevice>>::DeviceId,
        >,
        body: S,
    ) -> Result<(), S> {
        let src =
            <C as LinkDeviceHandler<EthernetLinkDevice>>::get_link_layer_addr(self, meta.device_id);
        self.send_frame(
            meta.device_id,
            body.encapsulate(EthernetFrameBuilder::new(src, meta.dst_addr, EtherType::Arp)),
        )
        .map_err(Nested::into_inner)
    }
}

impl<
        Id,
        C: InstantContext
            + StateContext<
                IpLinkDeviceState<
                    <C as InstantContext>::Instant,
                    EthernetDeviceState<<C as InstantContext>::Instant>,
                >,
                Id,
            >,
    > StateContext<NdpState<EthernetLinkDevice, <C as InstantContext>::Instant>, Id> for C
{
    fn get_state_with(
        &self,
        id: Id,
    ) -> &NdpState<EthernetLinkDevice, <C as InstantContext>::Instant> {
        &self.get_state_with(id).link().ndp
    }

    fn get_state_mut_with(
        &mut self,
        id: Id,
    ) -> &mut NdpState<EthernetLinkDevice, <C as InstantContext>::Instant> {
        &mut self.get_state_mut_with(id).link_mut().ndp
    }
}

/// An implementation of the [`LinkDevice`] trait for Ethernet devices.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub(super) struct EthernetLinkDevice;

impl LinkDevice for EthernetLinkDevice {
    type Address = Mac;
    type FrameType = EtherType;
}

#[cfg(test)]
mod tests {
    use net_types::ip::IpAddress;
    use net_types::SpecifiedAddr;
    use packet::Buf;
    use rand::Rng;
    use specialize_ip_macro::{ip_test, specialize_ip};

    use super::*;
    use crate::device::{is_routing_enabled, set_routing_enabled, DeviceId};
    use crate::ip::{
        dispatch_receive_ip_packet_name, receive_ip_packet, IpExt, IpPacketBuilder, IpProto,
    };
    use crate::testutil::{
        add_arp_or_ndp_table_entry, get_counter_val, get_dummy_config, get_other_ip_address,
        new_rng, parse_icmp_packet_in_ip_packet_in_ethernet_frame,
        parse_ip_packet_in_ethernet_frame, DummyEventDispatcher, DummyEventDispatcherBuilder,
        DUMMY_CONFIG_V4,
    };
    use crate::wire::icmp::{IcmpDestUnreachable, IcmpIpExt};
    use crate::wire::testdata::{dns_request_v4, dns_request_v6};
    use crate::StackStateBuilder;

    /*
    #[test]
    fn test_mtu() {
        // Test that we send an Ethernet frame whose size is less than the MTU,
        // and that we don't send an Ethernet frame whose size is greater than
        // the MTU.
        fn test(size: usize, expect_frames_sent: usize) {
            let mut ctx = DummyEventDispatcherBuilder::from_config(DUMMY_CONFIG_V4)
                .build::<DummyEventDispatcher>();
            let _ = send_ip_frame(
                &mut ctx,
                0.into(),
                DUMMY_CONFIG_V4.remote_ip,
                Buf::new(&mut vec![0; size], ..),
            );
            assert_eq!(ctx.dispatcher().frames_sent().len(), expect_frames_sent);
        }

        // The Ethernet device MTU currently defaults to IPV6_MIN_MTU.
        test(crate::ip::IPV6_MIN_MTU as usize, 1);
        test(crate::ip::IPV6_MIN_MTU as usize + 1, 0);
    }

    #[test]
    fn test_pending_frames() {
        let mut state = EthernetDeviceStateBuilder::new(DUMMY_CONFIG_V4.local_mac, IPV6_MIN_MTU)
            .build::<DummyInstant>();
        let ip = IpAddr::V4(DUMMY_CONFIG_V4.local_ip.into_addr());
        state.add_pending_frame(ip, Buf::new(vec![1], ..));
        state.add_pending_frame(ip, Buf::new(vec![2], ..));
        state.add_pending_frame(ip, Buf::new(vec![3], ..));

        // check that we're accumulating correctly...
        assert_eq!(3, state.take_pending_frames(ip).unwrap().count());
        // ...and that take_pending_frames clears all the buffered data.
        assert!(state.take_pending_frames(ip).is_none());

        for i in 0..ETHERNET_MAX_PENDING_FRAMES {
            assert!(state.add_pending_frame(ip, Buf::new(vec![i as u8], ..)).is_none());
        }
        // check that adding more than capacity will drop the older buffers as
        // a proper FIFO queue.
        assert_eq!(0, state.add_pending_frame(ip, Buf::new(vec![255], ..)).unwrap().as_ref()[0]);
        assert_eq!(1, state.add_pending_frame(ip, Buf::new(vec![255], ..)).unwrap().as_ref()[0]);
        assert_eq!(2, state.add_pending_frame(ip, Buf::new(vec![255], ..)).unwrap().as_ref()[0]);
    }
    */

    #[specialize_ip]
    fn test_receive_ip_frame<I: Ip>(initialize: bool) {
        //
        // Should only receive a frame if the device is initialized
        //

        let config = get_dummy_config::<I::Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(config.local_mac, crate::ip::IPV6_MIN_MTU);

        #[ipv4]
        let mut bytes = dns_request_v4::ETHERNET_FRAME.bytes.to_vec();

        #[ipv6]
        let mut bytes = dns_request_v6::ETHERNET_FRAME.bytes.to_vec();

        let mac_bytes = config.local_mac.bytes();
        bytes[0..6].copy_from_slice(&mac_bytes);

        if initialize {
            crate::device::initialize_device(&mut ctx, device);
        }

        // Will panic if we do not initialize.
        crate::device::receive_frame(&mut ctx, device, Buf::new(bytes, ..));

        // If we did not initialize, we would not reach here since
        // `receive_frame` would have paniced.
        #[ipv4]
        assert_eq!(get_counter_val(&mut ctx, "receive_ipv4_packet"), 1);
        #[ipv6]
        assert_eq!(get_counter_val(&mut ctx, "receive_ipv6_packet"), 1);
    }

    #[ip_test]
    #[should_panic(expected = "assertion failed: is_device_initialized(ctx.state(), device)")]
    fn receive_frame_uninitialized<I: Ip>() {
        test_receive_ip_frame::<I>(false);
    }

    #[ip_test]
    fn receive_frame_initialized<I: Ip>() {
        test_receive_ip_frame::<I>(true);
    }

    #[specialize_ip]
    fn test_send_ip_frame<I: Ip>(initialize: bool) {
        //
        // Should only send a frame if the device is initialized
        //

        let config = get_dummy_config::<I::Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(config.local_mac, crate::ip::IPV6_MIN_MTU);

        #[ipv4]
        let mut bytes = dns_request_v4::ETHERNET_FRAME.bytes.to_vec();

        #[ipv6]
        let mut bytes = dns_request_v6::ETHERNET_FRAME.bytes.to_vec();

        let mac_bytes = config.local_mac.bytes();
        bytes[6..12].copy_from_slice(&mac_bytes);

        if initialize {
            crate::device::initialize_device(&mut ctx, device);
        }

        // Will panic if we do not initialize.
        let _ =
            crate::device::send_ip_frame(&mut ctx, device, config.remote_ip, Buf::new(bytes, ..));
    }

    #[ip_test]
    #[should_panic(expected = "assertion failed: is_device_usable(ctx.state(), device)")]
    fn test_send_frame_uninitialized<I: Ip>() {
        test_send_ip_frame::<I>(false);
    }

    #[ip_test]
    fn test_send_frame_initialized<I: Ip>() {
        test_send_ip_frame::<I>(true);
    }

    #[test]
    fn initialize_once() {
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device =
            ctx.state_mut().add_ethernet_device(DUMMY_CONFIG_V4.local_mac, crate::ip::IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);
    }

    #[test]
    #[should_panic(expected = "assertion failed: state.is_uninitialized()")]
    fn initialize_multiple() {
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device =
            ctx.state_mut().add_ethernet_device(DUMMY_CONFIG_V4.local_mac, crate::ip::IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        // Should panic since we are already initialized.
        crate::device::initialize_device(&mut ctx, device);
    }

    #[ip_test]
    fn test_set_ip_routing<I: Ip + IcmpIpExt + IpExt>() {
        #[specialize_ip]
        fn check_other_is_routing_enabled<I: Ip>(
            ctx: &Context<DummyEventDispatcher>,
            device: DeviceId,
            expected: bool,
        ) {
            #[ipv4]
            assert_eq!(is_routing_enabled::<_, Ipv6>(ctx, device), expected);

            #[ipv6]
            assert_eq!(is_routing_enabled::<_, Ipv4>(ctx, device), expected);
        }

        #[specialize_ip]
        fn check_icmp<I: Ip>(buf: &[u8]) {
            #[ipv4]
            let (src_mac, dst_mac, src_ip, dst_ip, ttl, message, code) =
                parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv4, _, IcmpDestUnreachable, _>(
                    buf,
                    |_| {},
                )
                .unwrap();

            #[ipv6]
            let (src_mac, dst_mac, src_ip, dst_ip, ttl, message, code) =
                parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, IcmpDestUnreachable, _>(
                    buf,
                    |_| {},
                )
                .unwrap();
        }

        let src_ip = get_other_ip_address::<I::Addr>(3);
        let src_mac = Mac::new([10, 11, 12, 13, 14, 15]);
        let config = get_dummy_config::<I::Addr>();
        let device = DeviceId::new_ethernet(0);
        let frame_dst = FrameDestination::Unicast;
        let mut rng = new_rng(70812476915813);
        let mut body: Vec<u8> = std::iter::repeat_with(|| rng.gen()).take(100).collect();
        let buf = Buf::new(&mut body[..], ..)
            .encapsulate(I::PacketBuilder::new(
                src_ip.get(),
                config.remote_ip.get(),
                64,
                IpProto::Tcp,
            ))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .unwrap_b();

        //
        // Test with netstack no fowarding
        //

        let mut builder = DummyEventDispatcherBuilder::from_config(config.clone());
        add_arp_or_ndp_table_entry(&mut builder, device.id(), src_ip.get(), src_mac);
        let mut ctx = builder.build();

        // Should not be a router (default).
        assert!(!is_routing_enabled::<_, I>(&ctx, device));
        check_other_is_routing_enabled::<I>(&ctx, device, false);

        // Receiving a packet not destined for the node should result in a dest unreachable message.
        receive_ip_packet::<_, _, I>(&mut ctx, device, frame_dst, buf.clone());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        check_icmp::<I>(&ctx.dispatcher().frames_sent()[0].1);

        // Attempting to set router should work, but it still won't be able to
        // route packets.
        set_routing_enabled::<_, I>(&mut ctx, device, true);
        assert!(is_routing_enabled::<_, I>(&ctx, device));
        // Should not update other Ip routing status.
        check_other_is_routing_enabled::<I>(&ctx, device, false);
        receive_ip_packet::<_, _, I>(&mut ctx, device, frame_dst, buf.clone());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);
        check_icmp::<I>(&ctx.dispatcher().frames_sent()[1].1);

        //
        // Test with netstack fowarding
        //

        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv4_builder().forward(true);
        state_builder.ipv6_builder().forward(true);
        // Most tests do not need NDP's DAD or router solicitation so disable it here.
        let mut ndp_configs = ndp::NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(None);
        state_builder.device_builder().set_default_ndp_configs(ndp_configs);
        let mut builder = DummyEventDispatcherBuilder::from_config(config.clone());
        add_arp_or_ndp_table_entry(&mut builder, device.id(), src_ip.get(), src_mac);
        let mut ctx = builder.build_with(state_builder, DummyEventDispatcher::default());

        // Should not be a router (default).
        assert!(!is_routing_enabled::<_, I>(&ctx, device));
        check_other_is_routing_enabled::<I>(&ctx, device, false);

        // Receiving a packet not destined for the node should result in a dest unreachable message.
        receive_ip_packet::<_, _, I>(&mut ctx, device, frame_dst, buf.clone());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        check_icmp::<I>(&ctx.dispatcher().frames_sent()[0].1);

        // Attempting to set router should work
        set_routing_enabled::<_, I>(&mut ctx, device, true);
        assert!(is_routing_enabled::<_, I>(&ctx, device));
        // Should not update other Ip routing status.
        check_other_is_routing_enabled::<I>(&ctx, device, false);

        // Should route the packet since routing fully enabled (netstack & device).
        receive_ip_packet::<_, _, I>(&mut ctx, device, frame_dst, buf.clone());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);
        println!("{:?}", buf.as_ref());
        println!("{:?}", ctx.dispatcher().frames_sent()[1].1);
        let (packet_buf, _, _, packet_src_ip, packet_dst_ip, proto, ttl) =
            parse_ip_packet_in_ethernet_frame::<I>(&ctx.dispatcher().frames_sent()[1].1[..])
                .unwrap();
        assert_eq!(src_ip.get(), packet_src_ip);
        assert_eq!(config.remote_ip.get(), packet_dst_ip);
        assert_eq!(proto, IpProto::Tcp);
        assert_eq!(body, packet_buf);
        assert_eq!(ttl, 63);

        // Attempt to unset router
        set_routing_enabled::<_, I>(&mut ctx, device, false);
        assert!(!is_routing_enabled::<_, I>(&ctx, device));
        check_other_is_routing_enabled::<I>(&ctx, device, false);

        // Should not route packets anymore
        receive_ip_packet::<_, _, I>(&mut ctx, device, frame_dst, buf.clone());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 3);
        check_icmp::<I>(&ctx.dispatcher().frames_sent()[2].1);
    }

    #[ip_test]
    fn test_promiscuous_mode<I: Ip + IpExt>() {
        //
        // Test that frames not destined for a device will still be accepted when
        // the device is put into promiscuous mode. In all cases, frames that are
        // destined for a device must always be accepted.
        //

        let config = get_dummy_config::<I::Addr>();
        let mut ctx = DummyEventDispatcherBuilder::from_config(config.clone())
            .build::<DummyEventDispatcher>();
        let device = DeviceId::new_ethernet(0);
        let other_mac = Mac::new([13, 14, 15, 16, 17, 18]);

        let buf = Buf::new(Vec::new(), ..)
            .encapsulate(I::PacketBuilder::new(
                config.remote_ip.get(),
                config.local_ip.get(),
                64,
                IpProto::Tcp,
            ))
            .encapsulate(EthernetFrameBuilder::new(
                config.remote_mac,
                config.local_mac,
                I::ETHER_TYPE,
            ))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .unwrap_b();

        // Accept packet destined for this device if promiscuous mode is off.
        crate::device::set_promiscuous_mode(&mut ctx, device, false);
        crate::device::receive_frame(&mut ctx, device, buf.clone());
        assert_eq!(get_counter_val(&mut ctx, dispatch_receive_ip_packet_name::<I>()), 1);

        // Accept packet destined for this device if promiscuous mode is on.
        crate::device::set_promiscuous_mode(&mut ctx, device, true);
        crate::device::receive_frame(&mut ctx, device, buf.clone());
        assert_eq!(get_counter_val(&mut ctx, dispatch_receive_ip_packet_name::<I>()), 2);

        let buf = Buf::new(Vec::new(), ..)
            .encapsulate(I::PacketBuilder::new(
                config.remote_ip.get(),
                config.local_ip.get(),
                64,
                IpProto::Tcp,
            ))
            .encapsulate(EthernetFrameBuilder::new(config.remote_mac, other_mac, I::ETHER_TYPE))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .unwrap_b();

        // Reject packet not destined for this device if promiscuous mode is off.
        crate::device::set_promiscuous_mode(&mut ctx, device, false);
        crate::device::receive_frame(&mut ctx, device, buf.clone());
        assert_eq!(get_counter_val(&mut ctx, dispatch_receive_ip_packet_name::<I>()), 2);

        // Accept packet not destined for this device if promiscuous mode is on.
        crate::device::set_promiscuous_mode(&mut ctx, device, true);
        crate::device::receive_frame(&mut ctx, device, buf.clone());
        assert_eq!(get_counter_val(&mut ctx, dispatch_receive_ip_packet_name::<I>()), 3);
    }

    #[ip_test]
    fn test_add_remove_ip_addresses<I: Ip>() {
        let config = get_dummy_config::<I::Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(config.local_mac, crate::ip::IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        let ip1 = get_other_ip_address::<I::Addr>(1);
        let ip2 = get_other_ip_address::<I::Addr>(2);
        let ip3 = get_other_ip_address::<I::Addr>(3);

        let prefix = I::Addr::BYTES * 8;
        let as1 = AddrSubnet::new(ip1.get(), prefix).unwrap();
        let as2 = AddrSubnet::new(ip2.get(), prefix).unwrap();

        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip3).is_none());

        // Add ip1 (ok)
        crate::device::add_ip_addr_subnet(&mut ctx, device, as1).unwrap();
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_some());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip3).is_none());

        // Add ip2 (ok)
        crate::device::add_ip_addr_subnet(&mut ctx, device, as2).unwrap();
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_some());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_some());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip3).is_none());

        // Del ip1 (ok)
        crate::device::del_ip_addr(&mut ctx, device, &ip1).unwrap();
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_some());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip3).is_none());

        // Del ip1 again (ip1 not found)
        assert_eq!(
            crate::device::del_ip_addr(&mut ctx, device, &ip1).unwrap_err(),
            AddressError::NotFound
        );
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_some());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip3).is_none());

        // Add ip2 again (ip2 already exists)
        assert_eq!(
            crate::device::add_ip_addr_subnet(&mut ctx, device, as2).unwrap_err(),
            AddressError::AlreadyExists
        );
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_some());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip3).is_none());

        // Add ip2 with different subnet (ip2 already exists)
        assert_eq!(
            crate::device::add_ip_addr_subnet(
                &mut ctx,
                device,
                AddrSubnet::new(ip2.get(), prefix - 1).unwrap()
            )
            .unwrap_err(),
            AddressError::AlreadyExists
        );
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_some());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip3).is_none());
    }

    fn receive_simple_ip_packet_test<A: IpAddress>(
        ctx: &mut Context<DummyEventDispatcher>,
        device: DeviceId,
        src_ip: A,
        dst_ip: A,
        expected: usize,
    ) {
        let buf = Buf::new(Vec::new(), ..)
            .encapsulate(<A::Version as IpExt>::PacketBuilder::new(
                src_ip,
                dst_ip,
                64,
                IpProto::Tcp,
            ))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .into_inner();

        receive_ip_packet::<_, _, A::Version>(ctx, device, FrameDestination::Unicast, buf);
        assert_eq!(get_counter_val(ctx, dispatch_receive_ip_packet_name::<A::Version>()), expected);
    }

    #[ip_test]
    fn test_multiple_ip_addresses<I: Ip>() {
        let config = get_dummy_config::<I::Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(config.local_mac, crate::ip::IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        let ip1 = get_other_ip_address::<I::Addr>(1);
        let ip2 = get_other_ip_address::<I::Addr>(2);
        let from_ip = get_other_ip_address::<I::Addr>(3).get();

        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_none());

        // Should not receive packets on any ip.
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip1.get(), 0);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip2.get(), 0);

        // Add ip1 to device.
        crate::device::add_ip_addr_subnet(
            &mut ctx,
            device,
            AddrSubnet::new(ip1.get(), I::Addr::BYTES * 8).unwrap(),
        )
        .unwrap();
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).unwrap().is_assigned());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).is_none());

        // Should receive packets on ip1 but not ip2
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip1.get(), 1);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip2.get(), 1);

        // Add ip2 to device.
        crate::device::add_ip_addr_subnet(
            &mut ctx,
            device,
            AddrSubnet::new(ip2.get(), I::Addr::BYTES * 8).unwrap(),
        )
        .unwrap();
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).unwrap().is_assigned());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).unwrap().is_assigned());

        // Should receive packets on both ips
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip1.get(), 2);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip2.get(), 3);

        // Remove ip1
        crate::device::del_ip_addr(&mut ctx, device, &ip1).unwrap();
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip1).is_none());
        assert!(crate::device::get_ip_addr_state(&ctx, device, &ip2).unwrap().is_assigned());

        // Should receive packets on ip2
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip1.get(), 3);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip2.get(), 4);
    }

    /// Get a multicast address.
    #[specialize_ip]
    fn get_multicast_addr<I: Ip>() -> MulticastAddr<I::Addr> {
        #[ipv4]
        return MulticastAddr::new(Ipv4Addr::new([224, 0, 0, 1])).unwrap();

        #[ipv6]
        return MulticastAddr::new(Ipv6Addr::new([
            0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
        ]))
        .unwrap();
    }

    /// Test that we can join and leave a multicast group, but we only truly leave it after
    /// calling `leave_ip_multicast` the same number of times as `join_ip_multicast`.
    #[ip_test]
    fn test_ip_join_leave_multicast_addr_ref_count<I: Ip>() {
        let config = get_dummy_config::<I::Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(config.local_mac, crate::ip::IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        let multicast_addr = get_multicast_addr::<I>();

        // Should not be in the multicast group yet.
        assert!(!crate::device::is_in_ip_multicast(&mut ctx, device, multicast_addr));

        // Join the multicast group.
        crate::device::join_ip_multicast(&mut ctx, device, multicast_addr);
        assert!(crate::device::is_in_ip_multicast(&mut ctx, device, multicast_addr));

        // Leave the multicast group.
        crate::device::leave_ip_multicast(&mut ctx, device, multicast_addr);
        assert!(!crate::device::is_in_ip_multicast(&mut ctx, device, multicast_addr));

        // Join the multicst group.
        crate::device::join_ip_multicast(&mut ctx, device, multicast_addr);
        assert!(crate::device::is_in_ip_multicast(&mut ctx, device, multicast_addr));

        // Join it again...
        crate::device::join_ip_multicast(&mut ctx, device, multicast_addr);
        assert!(crate::device::is_in_ip_multicast(&mut ctx, device, multicast_addr));

        // Leave it (still in it because we joined twice).
        crate::device::leave_ip_multicast(&mut ctx, device, multicast_addr);
        assert!(crate::device::is_in_ip_multicast(&mut ctx, device, multicast_addr));

        // Leave it again... (actually left now).
        crate::device::leave_ip_multicast(&mut ctx, device, multicast_addr);
        assert!(!crate::device::is_in_ip_multicast(&mut ctx, device, multicast_addr));
    }

    /// Test leaving a multicast group a device has not yet joined.
    ///
    /// # Panics
    ///
    /// This method should always panic as leaving an unjoined multicast group is a panic
    /// condition.
    #[ip_test]
    #[should_panic(expected = "cannot leave not-yet-joined multicast group")]
    fn test_ip_leave_unjoined_multicast<I: Ip>() {
        let config = get_dummy_config::<I::Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(config.local_mac, crate::ip::IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        let multicast_addr = get_multicast_addr::<I>();

        // Should not be in the multicast group yet.
        assert!(!crate::device::is_in_ip_multicast(&mut ctx, device, multicast_addr));

        // Leave it (this should panic).
        crate::device::leave_ip_multicast(&mut ctx, device, multicast_addr);
    }

    #[test]
    fn test_ipv6_duplicate_solicited_node_address() {
        //
        // Test that we still receive packets destined to a solicited-node multicast address of an
        // IP address we deleted because another (distinct) IP address that is still assigned uses
        // the same solicited-node multicast address.
        //

        let config = get_dummy_config::<Ipv6Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(config.local_mac, crate::ip::IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        let ip1 =
            SpecifiedAddr::new(Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1]))
                .unwrap();
        let ip2 =
            SpecifiedAddr::new(Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1]))
                .unwrap();
        let from_ip = Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 1]);

        // ip1 and ip2 are not equal but their solicited node addresses are the same.
        assert_ne!(ip1, ip2);
        assert_eq!(ip1.to_solicited_node_address(), ip2.to_solicited_node_address());
        let sn_addr = ip1.to_solicited_node_address().get();

        let addr_sub1 = AddrSubnet::new(ip1.get(), 64).unwrap();
        let addr_sub2 = AddrSubnet::new(ip2.get(), 64).unwrap();

        assert_eq!(get_counter_val(&mut ctx, "dispatch_receive_ip_packet"), 0);

        // Add ip1 to the device.
        //
        // Should get packets destined for the solicited node address and ip1.
        crate::device::add_ip_addr_subnet(&mut ctx, device, addr_sub1).unwrap();
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip1.get(), 1);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip2.get(), 1);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, sn_addr, 2);

        // Add ip2 to the device.
        //
        // Should get packets destined for the solicited node address, ip1 and ip2.
        crate::device::add_ip_addr_subnet(&mut ctx, device, addr_sub2).unwrap();
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip1.get(), 3);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip2.get(), 4);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, sn_addr, 5);

        // Remove ip1 from the device.
        //
        // Should get packets destined for the solicited node address and ip2.
        crate::device::del_ip_addr(&mut ctx, device, &ip1).unwrap();
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip1.get(), 5);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, ip2.get(), 6);
        receive_simple_ip_packet_test(&mut ctx, device, from_ip, sn_addr, 7);
    }
}
