// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::{HashMap, VecDeque};
use std::marker::PhantomData;
use std::num::NonZeroU8;
use std::slice::Iter;

use log::{debug, trace};
use net_types::ethernet::Mac;
use net_types::ip::{AddrSubnet, Ip, IpAddr, IpAddress, IpVersion, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr};
use net_types::{MulticastAddr, SpecifiedAddr, Witness};
use never::Never;
use packet::{Buf, BufferMut, EmptyBuf, Serializer};

use crate::context::{
    CounterContext, FrameContext, InstantContext, RecvFrameContext, RngContext, StateContext,
    TimerContext,
};
use crate::device::arp::{
    self, ArpContext, ArpDevice, ArpDeviceIdContext, ArpFrameMetadata, ArpHandler,
    ArpPacketHandler, ArpState, ArpTimerId, HType,
};
use crate::device::ip::{IpDeviceHandler, IpDeviceHandlerPrivate, IpDeviceIdContext};
use crate::device::link::{
    BufferLinkDeviceHandler, LinkDevice, LinkDeviceHandler, LinkDeviceIdContext, LinkFrameMeta,
};
use crate::device::ndp::{NdpContext, NdpHandler, NdpState, NdpTimerId};
use crate::device::{
    AddressConfigurationType, AddressEntry, AddressError, AddressState, IpDeviceState,
    RecvIpFrameMeta, Tentative,
};
use crate::ip::IpHandler;
use crate::{Context, EventDispatcher, Instant};

#[derive(Default)]
pub(super) struct IpLinkDeviceStuff {
    // pending_frames stores a list of serialized frames indexed by their
    // desintation IP addresses. The frames contain an entire EthernetFrame
    // body and the MTU check is performed before queueing them here.
    pending_frames: HashMap<IpAddr, VecDeque<Buf<Vec<u8>>>>,
}

/// State for a link-device that is also an IP device.
///
/// D is the link-specific state.
pub(super) struct IpLinkDeviceState<I: Instant, D> {
    ip: IpDeviceState<I>,
    link: D,
    // TODO(ghanan): rename
    stuff: IpLinkDeviceStuff,
}

impl<I: Instant, D> IpLinkDeviceState<I, D> {
    /// Create a new `IpLinkDeviceState` with a link-specific state `link`.
    pub(super) fn new(link: D) -> Self {
        Self { ip: IpDeviceState::default(), link, stuff: IpLinkDeviceStuff::default() }
    }

    /// Get a reference to the ip (link-independant) state.
    pub(super) fn ip(&self) -> &IpDeviceState<I> {
        &self.ip
    }

    /// Get a mutable reference to the ip (link-independant) state.
    pub(super) fn ip_mut(&mut self) -> &mut IpDeviceState<I> {
        &mut self.ip
    }

    /// Get a reference to the inner (link-specific) state.
    pub(super) fn link(&self) -> &D {
        &self.link
    }

    /// Get a mutable reference to the inner (link-specific) state.
    pub(super) fn link_mut(&mut self) -> &mut D {
        &mut self.link
    }

    /// Get a reference to the ip-link state.
    pub(super) fn ip_link(&self) -> &IpLinkDeviceStuff {
        &self.stuff
    }

    /// Get a mutable reference to the ip-link state.
    pub(super) fn ip_link_mut(&mut self) -> &mut IpLinkDeviceStuff {
        &mut self.stuff
    }
}

/// A timer ID for IPv4 link devices.
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub(crate) enum Ipv4LinkDeviceTimerId<D: IpLinkDevice, Id> {
    Arp(ArpTimerId<D, Ipv4Addr, Id>),
}

impl<D: IpLinkDevice, Id> From<ArpTimerId<D, Ipv4Addr, Id>> for Ipv4LinkDeviceTimerId<D, Id> {
    fn from(id: ArpTimerId<D, Ipv4Addr, Id>) -> Ipv4LinkDeviceTimerId<D, Id> {
        Ipv4LinkDeviceTimerId::Arp(id)
    }
}

impl_timer_context!(
    D,
    IpLinkDevice,
    IpLinkDeviceIdContext<D>,
    Ipv4LinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
    ArpTimerId<D, Ipv4Addr, <C as LinkDeviceIdContext<D>>::DeviceId>,
    Ipv4LinkDeviceTimerId::Arp(id),
    id
);

/// A timer ID for IPv6 link devices.
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub(crate) enum Ipv6LinkDeviceTimerId<D: IpLinkDevice, Id> {
    Ndp(NdpTimerId<D, Id>),
}

impl<D: IpLinkDevice, Id> From<NdpTimerId<D, Id>> for Ipv6LinkDeviceTimerId<D, Id> {
    fn from(id: NdpTimerId<D, Id>) -> Ipv6LinkDeviceTimerId<D, Id> {
        Ipv6LinkDeviceTimerId::Ndp(id)
    }
}

impl_timer_context!(
    D,
    IpLinkDevice,
    IpLinkDeviceIdContext<D>,
    Ipv6LinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
    NdpTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
    Ipv6LinkDeviceTimerId::Ndp(id),
    id
);

#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub(crate) enum IpLinkDeviceTimerId<D: IpLinkDevice, Id> {
    V4(Ipv4LinkDeviceTimerId<D, Id>),
    V6(Ipv6LinkDeviceTimerId<D, Id>),
}

impl<D: IpLinkDevice, Id> From<Ipv4LinkDeviceTimerId<D, Id>> for IpLinkDeviceTimerId<D, Id> {
    fn from(id: Ipv4LinkDeviceTimerId<D, Id>) -> IpLinkDeviceTimerId<D, Id> {
        IpLinkDeviceTimerId::V4(id)
    }
}

impl<D: IpLinkDevice, Id> From<Ipv6LinkDeviceTimerId<D, Id>> for IpLinkDeviceTimerId<D, Id> {
    fn from(id: Ipv6LinkDeviceTimerId<D, Id>) -> IpLinkDeviceTimerId<D, Id> {
        IpLinkDeviceTimerId::V6(id)
    }
}

impl_timer_context!(
    D,
    IpLinkDevice,
    IpLinkDeviceIdContext<D>,
    IpLinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
    Ipv4LinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
    IpLinkDeviceTimerId::V4(id),
    id
);

impl_timer_context!(
    D,
    IpLinkDevice,
    IpLinkDeviceIdContext<D>,
    IpLinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
    Ipv6LinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
    IpLinkDeviceTimerId::V6(id),
    id
);

// TODO(ghanan): Split this up into IPv4 and IPv6 specific traits so only the IPv4 type
// needs to be an `ArpDevice`.
pub(crate) trait IpLinkDevice: LinkDevice + ArpDevice {
    /// Returns a link address for an IP address if there is a direct mapping available
    /// without a link address resolution process.
    fn ip_to_link<A: IpAddress>(a: A) -> Option<Self::Address>;
}

impl<C: LinkDevice<Address = Mac>> IpLinkDevice for C {
    fn ip_to_link<A: IpAddress>(a: A) -> Option<Mac> {
        match MulticastAddr::new(a) {
            Some(multicast) => Some(Mac::from(&multicast)),
            None => None,
        }
    }
}

pub(crate) trait IpLinkDeviceIdContext<D: IpLinkDevice>:
    LinkDeviceIdContext<D>
    + IpDeviceIdContext
    + ArpDeviceIdContext<D, DeviceId = <Self as LinkDeviceIdContext<D>>::DeviceId>
{
    /// Given a link-device specific device ID, return the equivalent IP device ID.
    fn link_device_id_to_ip_device_id(
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
    ) -> <Self as IpDeviceIdContext>::DeviceId;
}

impl<
        D: IpLinkDevice,
        T: LinkDeviceIdContext<D>
            + IpDeviceIdContext
            + ArpDeviceIdContext<D, DeviceId = <T as LinkDeviceIdContext<D>>::DeviceId>,
    > IpLinkDeviceIdContext<D> for T
where
    <T as IpDeviceIdContext>::DeviceId: From<<T as LinkDeviceIdContext<D>>::DeviceId>,
{
    fn link_device_id_to_ip_device_id(
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
    ) -> <Self as IpDeviceIdContext>::DeviceId {
        <Self as IpDeviceIdContext>::DeviceId::from(device)
    }
}

pub(super) trait IpLinkDeviceContextImpl<I: Ip, D: IpLinkDevice>:
    IpLinkDeviceIdContext<D>
{
    type TimerId;
}

impl<I: Ip, D: IpLinkDevice, C: IpLinkDeviceIdContext<D>> IpLinkDeviceContextImpl<I, D> for C {
    default type TimerId = Never;
}

impl<D: IpLinkDevice, C: IpLinkDeviceIdContext<D>> IpLinkDeviceContextImpl<Ipv4, D> for C {
    type TimerId = Ipv4LinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>;
}

impl<D: IpLinkDevice, C: IpLinkDeviceIdContext<D>> IpLinkDeviceContextImpl<Ipv6, D> for C {
    type TimerId = Ipv6LinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>;
}

/// The context provided by the device layer to a particular link device implementation
/// that is used for IP.
pub(super) trait IpLinkDeviceContext<I: Ip, D: IpLinkDevice>:
    IpLinkDeviceContextImpl<I, D>
    + IpHandler
    + LinkDeviceHandler<D>
    + IpDeviceHandler<I>
    + IpDeviceHandlerPrivate<I>
    + StateContext<super::IpLinkDeviceStuff, <Self as LinkDeviceIdContext<D>>::DeviceId>
    + FrameContext<EmptyBuf, <Self as LinkDeviceIdContext<D>>::DeviceId>
    + FrameContext<Buf<Vec<u8>>, <Self as LinkDeviceIdContext<D>>::DeviceId>
    + TimerContext<<Self as IpLinkDeviceContextImpl<I, D>>::TimerId>
{
}

impl<
        I: Ip,
        D: IpLinkDevice,
        C: LinkDeviceHandler<D>
            + IpDeviceHandler<I>
            + IpDeviceHandlerPrivate<I>
            + IpLinkDeviceContextImpl<I, D>
            + FrameContext<EmptyBuf, <C as LinkDeviceIdContext<D>>::DeviceId>
            + FrameContext<Buf<Vec<u8>>, <C as LinkDeviceIdContext<D>>::DeviceId>
            + StateContext<super::IpLinkDeviceStuff, <C as LinkDeviceIdContext<D>>::DeviceId>
            + TimerContext<<C as IpLinkDeviceContextImpl<I, D>>::TimerId>
            + IpHandler,
    > IpLinkDeviceContext<I, D> for C
{
}

// NOTE(joshlf): We know that this is safe because the Ip trait is sealed to
// only be implemented by Ipv4 and Ipv6.
impl<I: Ip, D: IpLinkDevice, C: IpLinkDeviceContextImpl<I, D> + LinkDeviceHandler<D>>
    IpLinkDeviceHandler<I, D> for C
{
    default type PendingFramesIter = Never;
    default const MAX_PENDING_FRAMES: usize = 0;

    default fn initialize_device(&mut self, _device: <Self as LinkDeviceIdContext<D>>::DeviceId) {
        unreachable!()
    }

    default fn deinitialize_device(&mut self, _device: <Self as LinkDeviceIdContext<D>>::DeviceId) {
        unreachable!()
    }

    default fn add_pending_frame(
        &mut self,
        _device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        _local_addr: I::Addr,
        _frame: Buf<Vec<u8>>,
    ) -> Option<Buf<Vec<u8>>> {
        unreachable!()
    }

    default fn take_pending_frames(
        &mut self,
        _device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        _local_addr: I::Addr,
    ) -> Option<Self::PendingFramesIter> {
        unreachable!()
    }

    default fn added_ip_addr(
        &mut self,
        _device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        _addr: SpecifiedAddr<I::Addr>,
    ) {
        unreachable!()
    }

    default fn removed_ip_addr(
        &mut self,
        _device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        _addr: SpecifiedAddr<I::Addr>,
    ) {
        unreachable!()
    }

    default fn set_routing_enabled(
        &mut self,
        _device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        _enabled: bool,
    ) {
        unreachable!()
    }

    default fn handle_timer(&mut self, _id: <Self as IpLinkDeviceContextImpl<I, D>>::TimerId) {
        unreachable!()
    }

    #[cfg(test)]
    default fn insert_static_neighbor(
        &mut self,
        _device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        _addr: I::Addr,
        _hw_addr: D::Address,
    ) {
        unreachable!()
    }
}

pub(super) trait IpLinkDeviceHandler<I: Ip, D: IpLinkDevice>:
    IpLinkDeviceContextImpl<I, D> + LinkDeviceHandler<D>
{
    type PendingFramesIter: Iterator<Item = Buf<Vec<u8>>>;
    const MAX_PENDING_FRAMES: usize;

    fn initialize_device(&mut self, device: <Self as LinkDeviceIdContext<D>>::DeviceId);

    fn deinitialize_device(&mut self, device: <Self as LinkDeviceIdContext<D>>::DeviceId);

    /// Adds a pending frame `frame` associated with `local_addr` to the list
    /// of pending frames in the current device state.
    ///
    /// If an older frame had to be dropped because it exceeds the maximum
    /// allowed number of pending frames, it is returned.
    fn add_pending_frame(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        local_addr: I::Addr,
        frame: Buf<Vec<u8>>,
    ) -> Option<Buf<Vec<u8>>>;

    /// Takes all pending frames associated with address `local_addr`.
    fn take_pending_frames(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        local_addr: I::Addr,
    ) -> Option<Self::PendingFramesIter>;

    fn added_ip_addr(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        addr: SpecifiedAddr<I::Addr>,
    );

    fn removed_ip_addr(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        addr: SpecifiedAddr<I::Addr>,
    );

    fn set_routing_enabled(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        enabled: bool,
    );

    /// Handle an IP link device timer firing.
    fn handle_timer(&mut self, id: <Self as IpLinkDeviceContextImpl<I, D>>::TimerId);

    /// Insert a static entry into this device's neighbor table.
    ///
    /// This will cause any conflicting dynamic entry to be removed, and
    /// any future conflicting gratuitous address resolution messages to
    /// be ignored.
    // TODO(rheacock): remove `cfg(test)` when this is used. Will probably be
    // called by a pub fn in the device mod.
    #[cfg(test)]
    fn insert_static_neighbor(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        addr: I::Addr,
        hw_addr: D::Address,
    );
}

impl<
        D: IpLinkDevice + ArpDevice<HType = <D as LinkDevice>::Address>,
        C: IpLinkDeviceContext<Ipv4, D>
            + ArpHandler<D, Ipv4Addr>
            + IpLinkDeviceContextImpl<
                Ipv4,
                D,
                TimerId = Ipv4LinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
            >,
    > IpLinkDeviceHandler<Ipv4, D> for C
where
    <D as LinkDevice>::Address: HType,
{
    type PendingFramesIter = std::collections::vec_deque::IntoIter<Buf<Vec<u8>>>;
    const MAX_PENDING_FRAMES: usize = 10;

    fn initialize_device(&mut self, device: <Self as LinkDeviceIdContext<D>>::DeviceId) {
        <C as IpDeviceHandlerPrivate<Ipv4>>::initialize_device_inner(
            self,
            C::link_device_id_to_ip_device_id(device),
        );
    }

    fn deinitialize_device(&mut self, device: <Self as LinkDeviceIdContext<D>>::DeviceId) {
        arp::deinitialize(self, device);
    }

    fn add_pending_frame(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        local_addr: Ipv4Addr,
        frame: Buf<Vec<u8>>,
    ) -> Option<Buf<Vec<u8>>> {
        add_pending_frame(self, device, local_addr.into(), frame)
    }

    fn take_pending_frames(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        local_addr: Ipv4Addr,
    ) -> Option<Self::PendingFramesIter> {
        take_pending_frame(self, device, local_addr.into())
    }

    fn added_ip_addr(
        &mut self,
        _device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        _addr: SpecifiedAddr<Ipv4Addr>,
    ) {
    }

    fn removed_ip_addr(
        &mut self,
        _device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        _addr: SpecifiedAddr<Ipv4Addr>,
    ) {
    }

    fn set_routing_enabled(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        enabled: bool,
    ) {
        if <C as IpDeviceHandler<Ipv4>>::is_routing_enabled(
            self,
            C::link_device_id_to_ip_device_id(device),
        ) == enabled
        {
            return;
        }

        <C as IpDeviceHandlerPrivate<Ipv4>>::set_routing_enabled_inner(
            self,
            C::link_device_id_to_ip_device_id(device),
            enabled,
        );
    }

    fn handle_timer(&mut self, id: <Self as IpLinkDeviceContextImpl<Ipv4, D>>::TimerId) {
        match id {
            Ipv4LinkDeviceTimerId::Arp(id) => arp::handle_timer(self, id),
        }
    }

    #[cfg(test)]
    fn insert_static_neighbor(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        addr: Ipv4Addr,
        hw_addr: D::Address,
    ) {
        arp::insert_static_neighbor(self, device, addr, hw_addr);
    }
}

impl<
        D: IpLinkDevice,
        C: IpLinkDeviceContext<Ipv6, D>
            + NdpHandler<D>
            + IpLinkDeviceContextImpl<
                Ipv6,
                D,
                TimerId = Ipv6LinkDeviceTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>,
            >,
    > IpLinkDeviceHandler<Ipv6, D> for C
{
    type PendingFramesIter = std::collections::vec_deque::IntoIter<Buf<Vec<u8>>>;
    const MAX_PENDING_FRAMES: usize = 10;

    fn initialize_device(&mut self, device: <Self as LinkDeviceIdContext<D>>::DeviceId) {
        <C as IpDeviceHandlerPrivate<Ipv6>>::initialize_device_inner(
            self,
            C::link_device_id_to_ip_device_id(device),
        );

        if <C as IpDeviceHandler<Ipv6>>::is_router_device(
            self,
            C::link_device_id_to_ip_device_id(device),
        ) {
            // If the device is operating as a router, and it is configured to be an advertising
            // interface, start sending periodic router advertisements.
            let start_advertising = <C as NdpHandler<D>>::get_configurations(self, device)
                .get_router_configurations()
                .get_should_send_advertisements();

            if start_advertising {
                <C as NdpHandler<D>>::start_advertising_interface(self, device)
            }
        } else {
            // RFC 4861 section 6.3.7, it implies only a host sends router solicitation messages.
            <C as NdpHandler<D>>::start_soliciting_routers(self, device)
        }
    }

    fn deinitialize_device(&mut self, device: <Self as LinkDeviceIdContext<D>>::DeviceId) {
        <C as NdpHandler<D>>::deinitialize(self, device);
    }

    fn add_pending_frame(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        local_addr: Ipv6Addr,
        frame: Buf<Vec<u8>>,
    ) -> Option<Buf<Vec<u8>>> {
        add_pending_frame(self, device, local_addr.into(), frame)
    }

    fn take_pending_frames(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        local_addr: Ipv6Addr,
    ) -> Option<Self::PendingFramesIter> {
        take_pending_frame(self, device, local_addr.into())
    }

    fn added_ip_addr(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        addr: SpecifiedAddr<Ipv6Addr>,
    ) {
        <C as NdpHandler<D>>::start_duplicate_address_detection(self, device, addr.get());
    }

    fn removed_ip_addr(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        addr: SpecifiedAddr<Ipv6Addr>,
    ) {
        <C as NdpHandler<D>>::cancel_duplicate_address_detection(self, device, addr.get());
    }

    fn set_routing_enabled(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        enabled: bool,
    ) {
        if <C as IpDeviceHandler<Ipv6>>::is_routing_enabled(
            self,
            C::link_device_id_to_ip_device_id(device),
        ) == enabled
        {
            return;
        }

        let ip_routing = <C as IpHandler>::is_routing_enabled::<Ipv6>(self);

        if enabled {
            trace!("set_ipv6_routing_enabled: enabling IPv6 routing for device {:?}", device);

            // Make sure that the netstack is configured to route packets before considering this
            // device a router and stopping router solicitations. If the netstack was not configured
            // to route packets before, then we would still be considered a host, so we shouldn't
            // stop soliciting routers.
            if ip_routing {
                // TODO(ghanan): Handle transition from disabled to enabled:
                //               - start periodic router advertisements (if configured to do so)
                <C as NdpHandler<D>>::stop_soliciting_routers(self, device)
            }

            // Actually update the routing flag.
            <C as IpDeviceHandlerPrivate<Ipv6>>::set_routing_enabled_inner(
                self,
                C::link_device_id_to_ip_device_id(device),
                true,
            );

            // Make sure that the netstack is configured to route packets before considering this
            // device a router and starting periodic router advertisements.
            if ip_routing
                && <C as IpDeviceHandler<Ipv6>>::get_link_local_addr(
                    self,
                    C::link_device_id_to_ip_device_id(device),
                )
                .is_some()
            {
                let should_send_advertisements =
                    <C as NdpHandler<D>>::get_configurations(self, device)
                        .get_router_configurations()
                        .get_should_send_advertisements();
                if should_send_advertisements {
                    <C as NdpHandler<D>>::start_advertising_interface(self, device);
                }
            }
        } else {
            trace!("set_ipv6_routing_enabled: disabling IPv6 routing for device {:?}", device);

            // Make sure that the netstack is configured to route packets before considering this
            // device a router and stopping periodic router advertisements. If the netstack was not
            // configured to route packets before, then we would still be considered a host, so we
            // wouldn't have any periodic router advertisements to stop.
            if ip_routing {
                // Make sure that the device was configured to send advertisements before stopping it.
                // If it was never configured to stop advertisements, there should be nothing to stop.
                if <C as IpDeviceHandler<Ipv6>>::get_link_local_addr(
                    self,
                    C::link_device_id_to_ip_device_id(device),
                )
                .is_some()
                {
                    let should_send_advertisements =
                        <C as NdpHandler<D>>::get_configurations(self, device)
                            .get_router_configurations()
                            .get_should_send_advertisements();
                    if should_send_advertisements {
                        <C as NdpHandler<D>>::stop_advertising_interface(self, device);
                    }
                }
            }

            // Actually update the routing flag.
            <C as IpDeviceHandlerPrivate<Ipv6>>::set_routing_enabled_inner(
                self,
                C::link_device_id_to_ip_device_id(device),
                false,
            );

            // We only need to start soliciting routers if we were not soliciting them before. We
            // would only reach this point if there was a change in routing status for `device`.
            // However, if the nestatck does not currently have routing enabled, the device would
            // not have been considered a router before this routing change on the device, so it
            // would have already solicited routers.
            if ip_routing {
                // On transition from router -> host, start soliciting router information.
                <C as NdpHandler<D>>::start_soliciting_routers(self, device)
            }
        }
    }

    fn handle_timer(&mut self, id: <Self as IpLinkDeviceContextImpl<Ipv6, D>>::TimerId) {
        match id {
            Ipv6LinkDeviceTimerId::Ndp(id) => <C as NdpHandler<D>>::handle_timer(self, id),
        }
    }

    #[cfg(test)]
    fn insert_static_neighbor(
        &mut self,
        device: <Self as LinkDeviceIdContext<D>>::DeviceId,
        addr: Ipv6Addr,
        hw_addr: D::Address,
    ) {
        <C as NdpHandler<_>>::insert_static_neighbor(self, device, addr, hw_addr)
    }
}

pub(super) fn handle_timer<
    I: Ip,
    D: IpLinkDevice,
    C: IpLinkDeviceHandler<I, D> + IpLinkDeviceContextImpl<I, D>,
>(
    ctx: &mut C,
    id: <C as IpLinkDeviceContextImpl<I, D>>::TimerId,
) {
    <C as IpLinkDeviceHandler<I, D>>::handle_timer(ctx, id)
}

#[cfg(test)]
pub(super) fn insert_static_neighbor<
    I: Ip,
    D: IpLinkDevice,
    C: IpLinkDeviceHandler<I, D> + IpLinkDeviceContextImpl<I, D>,
>(
    ctx: &mut C,
    device: <C as LinkDeviceIdContext<D>>::DeviceId,
    addr: I::Addr,
    hw_addr: D::Address,
) {
    <C as IpLinkDeviceHandler<I, D>>::insert_static_neighbor(ctx, device, addr, hw_addr)
}

/// `IpLinkDeviceContext` with an extra `B: BufferMut` parameter.
///
/// `BufferIpLinkDeviceContext` is used when sending a frame is required.
pub(super) trait BufferIpLinkDeviceContext<I: Ip, D: IpLinkDevice, B: BufferMut>:
    IpLinkDeviceContext<I, D>
    + ArpPacketHandler<D, Ipv4Addr, B>
    + BufferLinkDeviceHandler<D, B>
    + FrameContext<B, <Self as LinkDeviceIdContext<D>>::DeviceId>
    + RecvFrameContext<B, RecvIpFrameMeta<<Self as LinkDeviceIdContext<D>>::DeviceId, I>>
{
}

impl<
        I: Ip,
        D: IpLinkDevice,
        B: BufferMut,
        C: IpLinkDeviceContext<I, D>
            + ArpPacketHandler<D, Ipv4Addr, B>
            + BufferLinkDeviceHandler<D, B>
            + FrameContext<B, <Self as LinkDeviceIdContext<D>>::DeviceId>
            + RecvFrameContext<B, RecvIpFrameMeta<<Self as LinkDeviceIdContext<D>>::DeviceId, I>>,
    > BufferIpLinkDeviceContext<I, D, B> for C
{
}

pub(super) trait BufferIpLinkDeviceHandler<I: Ip, D: IpLinkDevice, B: BufferMut>:
    Sized
    + IpLinkDeviceHandler<I, D>
    + FrameContext<B, IpLinkFrameMeta<D, <Self as LinkDeviceIdContext<D>>::DeviceId, I::Addr>>
{
}

impl<E: EventDispatcher, A: IpAddress, D: IpLinkDevice, B: BufferMut>
    FrameContext<B, IpLinkFrameMeta<D, <Self as LinkDeviceIdContext<D>>::DeviceId, A>>
    for Context<E>
where
    Context<E>: IpLinkDeviceIdContext<D>,
{
    default fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        _meta: IpLinkFrameMeta<D, <Self as LinkDeviceIdContext<D>>::DeviceId, A>,
        _body: S,
    ) -> Result<(), S> {
        unreachable!()
    }
}

impl<
        E: EventDispatcher,
        D: IpLinkDevice + ArpDevice<HType = <D as LinkDevice>::Address>,
        B: BufferMut,
    > FrameContext<B, IpLinkFrameMeta<D, <Self as LinkDeviceIdContext<D>>::DeviceId, Ipv4Addr>>
    for Context<E>
where
    Context<E>:
        BufferLinkDeviceHandler<D, B> + IpLinkDeviceContext<Ipv4, D> + ArpHandler<D, Ipv4Addr>,
    <D as LinkDevice>::Address: HType,
{
    fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        meta: IpLinkFrameMeta<D, <Self as LinkDeviceIdContext<D>>::DeviceId, Ipv4Addr>,
        body: S,
    ) -> Result<(), S> {
        trace!(
            "ipv4link::send_ip_frame: local_addr = {:?}; device = {:?}",
            meta.dst_addr,
            meta.device
        );

        let local_mac =
            <Context<E> as LinkDeviceHandler<D>>::get_link_layer_addr(self, meta.device);
        let local_addr = meta.dst_addr.get();
        let dst_mac = match D::ip_to_link(local_addr) {
            Some(a) => Ok(a),
            None => {
                arp::lookup(self, meta.device, local_mac, local_addr).ok_or(local_addr).map(|x| x)
            }
        };

        send_ip_frame_inner(self, meta.device, local_addr, dst_mac.ok(), body)
    }
}

impl<E: EventDispatcher, D: IpLinkDevice, B: BufferMut>
    FrameContext<B, IpLinkFrameMeta<D, <Self as LinkDeviceIdContext<D>>::DeviceId, Ipv6Addr>>
    for Context<E>
where
    Context<E>: BufferLinkDeviceHandler<D, B> + IpLinkDeviceContext<Ipv6, D> + NdpHandler<D>,
{
    fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        meta: IpLinkFrameMeta<D, <Self as LinkDeviceIdContext<D>>::DeviceId, Ipv6Addr>,
        body: S,
    ) -> Result<(), S> {
        trace!(
            "ipv6link::send_ip_frame: local_addr = {:?}; device = {:?}",
            meta.dst_addr,
            meta.device
        );

        let local_addr = meta.dst_addr.get();
        let dst_mac = match D::ip_to_link(local_addr) {
            Some(a) => Ok(a),
            None => <Context<E> as NdpHandler<D>>::lookup(self, meta.device, local_addr)
                .ok_or(local_addr),
        };

        send_ip_frame_inner(self, meta.device, local_addr, dst_mac.ok(), body)
    }
}

impl<
        I: Ip,
        D: IpLinkDevice,
        B: BufferMut,
        C: IpLinkDeviceHandler<I, D>
            + FrameContext<B, IpLinkFrameMeta<D, <Self as LinkDeviceIdContext<D>>::DeviceId, I::Addr>>,
    > BufferIpLinkDeviceHandler<I, D, B> for C
{
}

impl<D: IpLinkDevice, C: LinkDeviceIdContext<D>> ArpDeviceIdContext<D> for C {
    type DeviceId = <C as LinkDeviceIdContext<D>>::DeviceId;
}

impl<D: IpLinkDevice + ArpDevice<HType = <D as LinkDevice>::Address>, C> ArpContext<D, Ipv4Addr>
    for C
where
    C: IpDeviceHandler<Ipv4>
        + IpLinkDeviceIdContext<D>
        + LinkDeviceHandler<D>
        + BufferLinkDeviceHandler<D, Buf<Vec<u8>>>
        + StateContext<super::IpLinkDeviceStuff, <C as LinkDeviceIdContext<D>>::DeviceId>
        + ArpDeviceIdContext<D>
        + StateContext<ArpState<D, Ipv4Addr>, <Self as ArpDeviceIdContext<D>>::DeviceId>
        + TimerContext<ArpTimerId<D, Ipv4Addr, <Self as ArpDeviceIdContext<D>>::DeviceId>>
        + FrameContext<EmptyBuf, ArpFrameMetadata<D, <Self as ArpDeviceIdContext<D>>::DeviceId>>
        + CounterContext,
    <D as LinkDevice>::Address: HType,
{
    fn get_protocol_addr(
        &self,
        device_id: <C as ArpDeviceIdContext<D>>::DeviceId,
    ) -> Option<Ipv4Addr> {
        <C as IpDeviceHandler<Ipv4>>::get_ip_addr_subnet(
            self,
            C::link_device_id_to_ip_device_id(device_id),
        )
        .map(|a| a.addr().get())
    }
    fn get_hardware_addr(&self, device_id: <C as ArpDeviceIdContext<D>>::DeviceId) -> D::HType {
        <C as LinkDeviceHandler<D>>::get_link_layer_addr(self, device_id)
    }
    fn address_resolved(
        &mut self,
        device_id: <C as ArpDeviceIdContext<D>>::DeviceId,
        proto_addr: Ipv4Addr,
        hw_addr: D::HType,
    ) {
        mac_resolved(self, device_id.into(), IpAddr::V4(proto_addr), hw_addr);
    }
    fn address_resolution_failed(
        &mut self,
        device_id: <C as ArpDeviceIdContext<D>>::DeviceId,
        proto_addr: Ipv4Addr,
    ) {
        mac_resolution_failed(self, device_id.into(), IpAddr::V4(proto_addr));
    }
    fn address_resolution_expired(
        &mut self,
        _device_id: <C as ArpDeviceIdContext<D>>::DeviceId,
        _proto_addr: Ipv4Addr,
    ) {
        log_unimplemented!((), "ArpContext::address_resolution_expired");
    }
}

impl<D: IpLinkDevice, C> NdpContext<D> for C
where
    C: IpDeviceHandler<Ipv6>
        + IpDeviceHandlerPrivate<Ipv6>
        + IpLinkDeviceIdContext<D>
        + LinkDeviceHandler<D>
        + FrameContext<
            Buf<Vec<u8>>,
            LinkFrameMeta<<C as LinkDeviceIdContext<D>>::DeviceId, D::Address, D::FrameType>,
        > + FrameContext<
            EmptyBuf,
            LinkFrameMeta<<C as LinkDeviceIdContext<D>>::DeviceId, D::Address, D::FrameType>,
        > + StateContext<super::IpLinkDeviceStuff, <C as LinkDeviceIdContext<D>>::DeviceId>
        + StateContext<
            super::IpDeviceState<<C as InstantContext>::Instant>,
            <C as IpDeviceIdContext>::DeviceId,
        > + RngContext
        + CounterContext
        + StateContext<
            NdpState<D, <C as InstantContext>::Instant>,
            <C as LinkDeviceIdContext<D>>::DeviceId,
        > + TimerContext<NdpTimerId<D, <C as LinkDeviceIdContext<D>>::DeviceId>>,
{
    fn get_link_layer_addr(
        &self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
    ) -> D::Address {
        <C as LinkDeviceHandler<D>>::get_link_layer_addr(self, device_id)
    }

    fn get_interface_identifier(
        &self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
    ) -> [u8; 8] {
        <C as LinkDeviceHandler<D>>::get_interface_identifier(self, device_id)
    }

    fn get_link_local_addr(
        &self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
    ) -> Option<Tentative<Ipv6Addr>> {
        let state: &IpDeviceState<C::Instant> =
            self.get_state_with(C::link_device_id_to_ip_device_id(device_id));
        state.ipv6_link_local_addr_sub.as_ref().map(|a| {
            if a.state().is_tentative() {
                Tentative::new_tentative(a.addr_sub().addr().get())
            } else {
                Tentative::new_permanent(a.addr_sub().addr().get())
            }
        })
    }

    fn get_ipv6_addr(
        &self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
    ) -> Option<Ipv6Addr> {
        // Return a non tentative global address, or the link-local address if no non-tentative
        // global addressses are associated with `device_id`.
        let ip_device_id = C::link_device_id_to_ip_device_id(device_id);
        match <C as IpDeviceHandler<Ipv6>>::get_ip_addr_subnet(self, ip_device_id) {
            Some(addr_sub) => Some(addr_sub.addr().get()),
            None => <C as IpDeviceHandler<Ipv6>>::get_link_local_addr(self, ip_device_id)
                .map(|a| a.get()),
        }
    }

    fn get_ipv6_addr_entries(
        &self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
    ) -> Iter<AddressEntry<Ipv6Addr, C::Instant>> {
        let state: &IpDeviceState<C::Instant> =
            self.get_state_with(C::link_device_id_to_ip_device_id(device_id));
        state.ipv6_addr_sub.iter()
    }

    fn ipv6_addr_state(
        &self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        address: &Ipv6Addr,
    ) -> Option<AddressState> {
        let address = SpecifiedAddr::new(*address)?;

        if let Some(state) = <C as IpDeviceHandler<Ipv6>>::get_ip_addr_state(
            self,
            C::link_device_id_to_ip_device_id(device_id),
            &address,
        ) {
            Some(state)
        } else {
            let state: &IpDeviceState<C::Instant> =
                self.get_state_with(C::link_device_id_to_ip_device_id(device_id));
            state
                .ipv6_link_local_addr_sub
                .as_ref()
                .map(|a| if a.addr_sub().addr().get() == *address { Some(a.state()) } else { None })
                .unwrap_or(None)
        }
    }

    fn address_resolved(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        address: &Ipv6Addr,
        link_address: D::Address,
    ) {
        mac_resolved(self, device_id, IpAddr::V6(*address), link_address);
    }

    fn address_resolution_failed(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        address: &Ipv6Addr,
    ) {
        mac_resolution_failed(self, device_id, IpAddr::V6(*address));
    }

    fn duplicate_address_detected(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        addr: Ipv6Addr,
    ) {
        let state: &mut IpDeviceState<C::Instant> =
            self.get_state_mut_with(C::link_device_id_to_ip_device_id(device_id));

        let original_size = state.ipv6_addr_sub.len();
        state.ipv6_addr_sub.retain(|x| x.addr_sub().addr().get() != addr);
        let new_size = state.ipv6_addr_sub.len();

        if original_size == new_size {
            // Address was not a global address, check link-local address.
            if state
                .ipv6_link_local_addr_sub
                .as_mut()
                .filter(|a| a.addr_sub().addr().get() == addr)
                .is_some()
            {
                state.ipv6_link_local_addr_sub = None;
            } else {
                panic!("Duplicate address not known, cannot be removed");
            }
        } else {
            assert_eq!(original_size - new_size, 1);
        }

        // Leave the the solicited-node multicast group.
        <C as IpDeviceHandler<Ipv6>>::leave_ip_multicast(
            self,
            C::link_device_id_to_ip_device_id(device_id),
            addr.to_solicited_node_address(),
        );

        // TODO: we need to pick a different address depending on what flow we are using.
    }

    fn unique_address_determined(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        addr: Ipv6Addr,
    ) {
        trace!(
            "ipv6link::unique_address_determined: device_id = {:?}; addr = {:?}",
            device_id,
            addr
        );

        let state: &mut IpDeviceState<C::Instant> =
            self.get_state_mut_with(C::link_device_id_to_ip_device_id(device_id));

        if let Some(entry) =
            state.ipv6_addr_sub.iter_mut().find(|a| a.addr_sub().addr().get() == addr)
        {
            entry.mark_permanent();
        } else if let Some(entry) =
            state.ipv6_link_local_addr_sub.as_mut().filter(|a| a.addr_sub().addr().get() == addr)
        {
            entry.mark_permanent();
        } else {
            panic!("Attempted to resolve an unknown tentative address");
        }
    }

    fn set_mtu(&mut self, device_id: <C as LinkDeviceIdContext<D>>::DeviceId, mtu: u32) {
        // TODO(ghanan): Should this new MTU be updated only from the netstack's perspective or
        //               be exposed to the device hardware?

        // `mtu` must not be less than the minimum IPv6 MTU.
        assert!(mtu >= crate::ip::path_mtu::IPV6_MIN_MTU);

        <C as LinkDeviceHandler<D>>::set_mtu(self, device_id, mtu);
    }

    fn set_hop_limit(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        hop_limit: NonZeroU8,
    ) {
        let state: &mut IpDeviceState<C::Instant> =
            self.get_state_mut_with(C::link_device_id_to_ip_device_id(device_id));
        state.ipv6_hop_limit = hop_limit;
    }

    fn add_slaac_addr_sub(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        addr_sub: AddrSubnet<Ipv6Addr>,
        valid_until: C::Instant,
    ) -> Result<(), AddressError> {
        trace!(
            "ipv6link::add_slaac_addr_sub: adding address {:?} on device {:?}",
            addr_sub,
            device_id
        );

        <C as IpDeviceHandlerPrivate<Ipv6>>::add_ip_addr_subnet_inner(
            self,
            C::link_device_id_to_ip_device_id(device_id),
            addr_sub,
            AddressConfigurationType::Slaac,
            Some(valid_until),
        )
    }

    fn deprecate_slaac_addr(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        addr: &Ipv6Addr,
    ) {
        trace!(
            "ipv6link::deprecate_slaac_addr: deprecating address {:?} on device {:?}",
            addr,
            device_id
        );

        let state: &mut IpDeviceState<C::Instant> =
            self.get_state_mut_with(C::link_device_id_to_ip_device_id(device_id));

        if let Some(entry) = state.ipv6_addr_sub.iter_mut().find(|a| {
            (a.addr_sub().addr().get() == *addr)
                && a.configuration_type() == AddressConfigurationType::Slaac
        }) {
            match entry.state {
                AddressState::Assigned => {
                    entry.state = AddressState::Deprecated;
                }
                AddressState::Tentative => {
                    trace!("ipv6link::deprecate_slaac_addr: invalidating the deprecated tentative address {:?} on device {:?}", addr, device_id);
                    // If `addr` is currently tentative on `device_id`, the address should simply
                    // be invalidated as new connections should not use a deprecated address,
                    // and we should have no existing connections using a tentative address.

                    // We must have had an invalidation timeout if we just attempted to deprecate.
                    assert!(<C as TimerContext<_>>::cancel_timer(
                        self,
                        NdpTimerId::new_invalidate_slaac_address(device_id, *addr).into()
                    )
                    .is_some());

                    Self::invalidate_slaac_addr(self, device_id, addr);
                }
                AddressState::Deprecated => unreachable!(
                    "We should never attempt to deprecate an already deprecated address"
                ),
            }
        } else {
            panic!("Address is not configured via SLAAC on this device");
        }
    }

    fn invalidate_slaac_addr(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        addr: &Ipv6Addr,
    ) {
        trace!(
            "ipv6link::invalidate_slaac_addr: invalidating address {:?} on device {:?}",
            addr,
            device_id
        );

        // `unwrap` will panic if `addr` is not an address configured via SLAAC on `device_id`.
        <C as IpDeviceHandlerPrivate<Ipv6>>::del_ip_addr_inner(
            self,
            C::link_device_id_to_ip_device_id(device_id),
            addr,
            Some(AddressConfigurationType::Slaac),
        )
        .unwrap();
    }

    fn update_slaac_addr_valid_until(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        addr: &Ipv6Addr,
        valid_until: C::Instant,
    ) {
        trace!(
            "ipv6link::update_slaac_addr_valid_until: updating address {:?}'s valid until instant to {:?} on device {:?}",
            addr,
            valid_until,
            device_id
        );

        let state: &mut IpDeviceState<C::Instant> =
            self.get_state_mut_with(C::link_device_id_to_ip_device_id(device_id));

        if let Some(entry) = state.ipv6_addr_sub.iter_mut().find(|a| {
            (a.addr_sub().addr().get() == *addr)
                && a.configuration_type() == AddressConfigurationType::Slaac
        }) {
            entry.valid_until = Some(valid_until);
        } else {
            panic!("Address is not configured via SLAAC on this device");
        }
    }

    fn is_router(&self, device_id: <C as LinkDeviceIdContext<D>>::DeviceId) -> bool {
        <C as IpDeviceHandler<Ipv6>>::is_router_device(
            self,
            C::link_device_id_to_ip_device_id(device_id),
        )
    }

    fn send_ipv6_frame<S: Serializer<Buffer = EmptyBuf>>(
        &mut self,
        device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
        local_addr: Ipv6Addr,
        dst_mac: Option<D::Address>,
        body: S,
    ) -> Result<(), S> {
        // `device_id` must not be uninitialized.
        assert!(self.is_device_usable(device_id));
        send_ip_frame_inner(self, device_id, local_addr, dst_mac, body)
    }
}

pub(super) struct IpLinkFrameMeta<D: IpLinkDevice, Id, A: IpAddress> {
    device: Id,
    dst_addr: SpecifiedAddr<A>,
    _m: PhantomData<D>,
}

impl<D: IpLinkDevice, Id, A: IpAddress> IpLinkFrameMeta<D, Id, A> {
    pub(super) fn new(device: Id, dst_addr: SpecifiedAddr<A>) -> IpLinkFrameMeta<D, Id, A> {
        IpLinkFrameMeta { device, dst_addr, _m: PhantomData }
    }
}

/// Send an IP packet in an Ethernet frame.
///
/// `send_ip_frame` accepts a device ID, a local IP address, and a
/// `SerializationRequest`. It computes the routing information and serializes
/// the request in a new Ethernet frame and sends it.
fn send_ip_frame_inner<
    D: IpLinkDevice,
    B: BufferMut,
    C: BufferLinkDeviceHandler<D, B>
        + LinkDeviceIdContext<D>
        + StateContext<super::IpLinkDeviceStuff, <C as LinkDeviceIdContext<D>>::DeviceId>,
    A: IpAddress,
    S: Serializer<Buffer = B>,
>(
    ctx: &mut C,
    device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
    local_addr: A,
    dst_mac: Option<D::Address>,
    body: S,
) -> Result<(), S> {
    match dst_mac {
        Some(dst_mac) => ctx.send_frame(
            LinkFrameMeta {
                device: device_id,
                dst_addr: dst_mac,
                // TODO(ghanan): use a type parameter instead of a value.
                link_specific: D::FrameType::from(A::Version::VERSION),
            },
            body,
        ),
        None => {
            // The `serialize_vec_outer` call returns an `Either<B,
            // Buf<Vec<u8>>`. We could naively call `.as_ref().to_vec()` on it,
            // but if it were the `Buf<Vec<u8>>` variant, we'd be unnecessarily
            // allocating a new `Vec` when we already have one. Instead, we
            // leave the `Buf<Vec<u8>>` variant as it is, and only convert the
            // `B` variant by calling `map_a`. That gives us an
            // `Either<Buf<Vec<u8>>, Buf<Vec<u8>>`, which we call `into_inner`
            // on to get a `Buf<Vec<u8>>`.
            let mtu = <C as LinkDeviceHandler<D>>::get_mtu(ctx, device_id);
            let frame = body
                .with_mtu(mtu as usize)
                .serialize_vec_outer()
                .map_err(|ser| ser.1.into_inner())?
                .map_a(|buffer| Buf::new(buffer.as_ref().to_vec(), ..))
                .into_inner();
            let dropped = add_pending_frame(ctx, device_id, local_addr.into(), frame);
            if let Some(_) = dropped {
                // TODO(brunodalbo): Is it ok to silently just let this drop? Or
                //  should the IP layer be notified in any way?
                log_unimplemented!((), "Ethernet dropped frame because ran out of allowable space");
            }
            Ok(())
        }
    }
}

/// Sends out any pending frames that are waiting for link layer address
/// resolution.
///
/// `mac_resolved` is the common logic used when a link layer address is
/// resolved either by ARP or NDP.
fn mac_resolved<
    D: IpLinkDevice,
    C: LinkDeviceHandler<D>
        + LinkDeviceIdContext<D>
        + BufferLinkDeviceHandler<D, Buf<Vec<u8>>>
        + StateContext<super::IpLinkDeviceStuff, <C as LinkDeviceIdContext<D>>::DeviceId>,
>(
    ctx: &mut C,
    device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
    address: IpAddr,
    dst_mac: D::Address,
) {
    let frame_type = match &address {
        IpAddr::V4(_) => D::FrameType::from(IpVersion::V4),
        IpAddr::V6(_) => D::FrameType::from(IpVersion::V6),
    };
    if let Some(pending) = take_pending_frame(ctx, device_id, address) {
        for frame in pending {
            // NOTE(brunodalbo): We already performed MTU checking when we
            //  saved the buffer waiting for address resolution. It should
            //  be noted that the MTU check back then didn't account for
            //  ethernet frame padding required by EthernetFrameBuilder,
            //  but that's fine (as it stands right now) because the MTU
            //  is guaranteed to be larger than an Ethernet minimum frame
            //  body size.
            let res = ctx.send_frame(
                LinkFrameMeta { device: device_id, dst_addr: dst_mac, link_specific: frame_type },
                frame,
            );
            if let Err(_) = res {
                // TODO(joshlf): Do we want to handle this differently?
                debug!("Failed to send pending frame; MTU changed since frame was queued");
            }
        }
    }
}

/// Clears out any pending frames that are waiting for link layer address
/// resolution.
///
/// `mac_resolution_failed` is the common logic used when a link layer address
/// fails to resolve either by ARP or NDP.
fn mac_resolution_failed<
    D: IpLinkDevice,
    C: LinkDeviceHandler<D>
        + LinkDeviceIdContext<D>
        + StateContext<super::IpLinkDeviceStuff, <C as LinkDeviceIdContext<D>>::DeviceId>,
>(
    ctx: &mut C,
    device_id: <C as LinkDeviceIdContext<D>>::DeviceId,
    address: IpAddr,
) {
    // TODO(brunodalbo) what do we do here in regards to the pending frames?
    //  NDP's RFC explicitly states unreachable ICMP messages must be generated:
    //  "If no Neighbor Advertisement is received after MAX_MULTICAST_SOLICIT
    //  solicitations, address resolution has failed. The sender MUST return
    //  ICMP destination unreachable indications with code 3
    //  (Address Unreachable) for each packet queued awaiting address
    //  resolution."
    //  For ARP, we don't have such a clear statement on the RFC, it would make
    //  sense to do the same thing though.
    if let Some(_) = take_pending_frame(ctx, device_id, address) {
        log_unimplemented!((), "ethernet mac resolution failed not implemented");
    }
}

fn add_pending_frame<
    D: IpLinkDevice,
    C: LinkDeviceIdContext<D>
        + StateContext<super::IpLinkDeviceStuff, <C as LinkDeviceIdContext<D>>::DeviceId>,
>(
    ctx: &mut C,
    device: <C as LinkDeviceIdContext<D>>::DeviceId,
    local_addr: IpAddr,
    frame: Buf<Vec<u8>>,
) -> Option<Buf<Vec<u8>>> {
    let state: &mut IpLinkDeviceStuff = ctx.get_state_mut_with(device);
    let buff = state.pending_frames.entry(local_addr).or_insert_with(Default::default);
    buff.push_back(frame);
    if buff.len() > 10 {
        buff.pop_front()
    } else {
        None
    }
}

fn take_pending_frame<
    D: IpLinkDevice,
    C: LinkDeviceIdContext<D>
        + StateContext<super::IpLinkDeviceStuff, <C as LinkDeviceIdContext<D>>::DeviceId>,
>(
    ctx: &mut C,
    device: <C as LinkDeviceIdContext<D>>::DeviceId,
    local_addr: IpAddr,
) -> Option<std::collections::vec_deque::IntoIter<Buf<Vec<u8>>>> {
    let state: &mut IpLinkDeviceStuff = ctx.get_state_mut_with(device);
    match state.pending_frames.remove(&local_addr) {
        Some(buff) => Some(buff.into_iter()),
        None => None,
    }
}
