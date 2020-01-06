// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The device layer.

pub(crate) mod arp;
pub(crate) mod ethernet;
pub(crate) mod ip;
pub(crate) mod iplink;
mod link;
pub(crate) mod ndp;

use std::collections::HashMap;
use std::fmt::{self, Debug, Display, Formatter};
use std::marker::PhantomData;
use std::num::NonZeroU8;

use log::{debug, trace};
use net_types::ethernet::Mac;
use net_types::ip::{AddrSubnet, Ip, IpAddress, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr};
use net_types::{LinkLocalAddr, MulticastAddr, SpecifiedAddr, Witness};
use packet::{BufferMut, Serializer};
use specialize_ip_macro::{specialize_ip, specialize_ip_address};
use zerocopy::ByteSlice;

use crate::context::{FrameContext, RecvFrameContext, StateContext};
use crate::data_structures::{IdMap, IdMapCollectionKey};
use crate::device::ethernet::{
    EthernetDeviceId, EthernetDeviceState, EthernetDeviceStateBuilder, EthernetLinkDevice,
    EthernetTimerId,
};
use crate::device::ip::{IpDeviceHandler, IpDeviceIdContext};
use crate::device::iplink::{IpLinkDeviceTimerId, IpLinkFrameMeta, IpLinkState};
use crate::device::link::{LinkDeviceContext, LinkDeviceHandler, LinkDeviceIdContext};
use crate::device::ndp::{NdpHandler, NdpPacketHandler};
use crate::ip::igmp::IgmpInterface;
use crate::ip::mld::MldInterface;
use crate::ip::socket::IpSockUpdate;
use crate::wire::icmp::ndp::NdpPacket;
use crate::{BufferDispatcher, Context, EventDispatcher, Instant, StackState};

pub(crate) fn receive_ndp_packet<D: EventDispatcher, B: ByteSlice>(
    ctx: &mut Context<D>,
    device_id: DeviceId,
    src_ip: Ipv6Addr,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    packet: NdpPacket<B>,
) {
    crate::device::ndp::receive_ndp_packet::<EthernetLinkDevice, _, _>(
        ctx,
        device_id.id.into(),
        src_ip,
        dst_ip,
        packet,
    )
}

struct RecvIpFrameMeta<D, I: Ip> {
    device: D,
    frame_dst: FrameDestination,
    _marker: PhantomData<I>,
}

impl<D, I: Ip> RecvIpFrameMeta<D, I> {
    fn new(device: D, frame_dst: FrameDestination) -> RecvIpFrameMeta<D, I> {
        RecvIpFrameMeta { device, frame_dst, _marker: PhantomData }
    }
}

impl<D: EventDispatcher> LinkDeviceContext<EthernetLinkDevice> for Context<D> {}

impl<D: EventDispatcher> LinkDeviceHandler<EthernetLinkDevice> for Context<D> {
    fn is_device_usable(&self, device: EthernetDeviceId) -> bool {
        is_device_usable(self.state(), device.into())
    }

    fn get_link_layer_addr(&self, device_id: EthernetDeviceId) -> Mac {
        <Context<_> as StateContext<IpLinkDeviceState<_, EthernetDeviceState<_>>, _>>
            ::get_state_with(self, device_id).link().mac
    }

    fn get_interface_identifier(&self, device_id: EthernetDeviceId) -> [u8; 8] {
        self.get_link_layer_addr(device_id).to_eui64()
    }

    fn set_mtu(&mut self, device_id: EthernetDeviceId, mut mtu: u32) {
        let dev_state = <Context<_> as StateContext<
            IpLinkDeviceState<_, EthernetDeviceState<_>>,
            _,
        >>::get_state_mut_with(self, device_id)
        .link_mut();

        // If `mtu` is greater than what the device supports, set `mtu` to the maximum MTU the
        // device supports.
        if mtu > dev_state.hw_mtu {
            trace!("set_mtu: MTU of {:?} is greater than the device {:?}'s max MTU of {:?}, using device's max MTU instead", mtu, device_id, dev_state.hw_mtu);
            mtu = dev_state.hw_mtu;
        }

        trace!("set_mtu: setting link MTU to {:?}", mtu);
        dev_state.mtu = mtu;
    }

    fn get_mtu(&self, device_id: EthernetDeviceId) -> u32 {
        <Context<_> as StateContext<
            IpLinkDeviceState<_, EthernetDeviceState<_>>,
            _,
        >>::get_state_with(self, device_id)
        .link().mtu
    }

    fn join_link_multicast(
        &mut self,
        device_id: EthernetDeviceId,
        multicast_addr: MulticastAddr<Mac>,
    ) {
        ethernet::join_link_multicast(self, device_id, multicast_addr)
    }

    fn leave_link_multicast(
        &mut self,
        device_id: EthernetDeviceId,
        multicast_addr: MulticastAddr<Mac>,
    ) {
        ethernet::leave_link_multicast(self, device_id, multicast_addr)
    }
}

impl<B: BufferMut, D: BufferDispatcher<B>>
    RecvFrameContext<B, RecvIpFrameMeta<EthernetDeviceId, Ipv4>> for Context<D>
{
    fn receive_frame(&mut self, metadata: RecvIpFrameMeta<EthernetDeviceId, Ipv4>, frame: B) {
        crate::ip::receive_ipv4_packet(self, metadata.device.into(), metadata.frame_dst, frame);
    }
}

impl<B: BufferMut, D: BufferDispatcher<B>>
    RecvFrameContext<B, RecvIpFrameMeta<EthernetDeviceId, Ipv6>> for Context<D>
{
    fn receive_frame(&mut self, metadata: RecvIpFrameMeta<EthernetDeviceId, Ipv6>, frame: B) {
        crate::ip::receive_ipv6_packet(self, metadata.device.into(), metadata.frame_dst, frame);
    }
}

/// State for a link-device that is also an IPv4 and IPv6 device.
///
/// D is the link-specific state.
struct IpLinkDeviceState<I: Instant, D> {
    ip: IpDeviceState<I>,
    link: D,
    iplink_v4: IpLinkState<Ipv4>,
    iplink_v6: IpLinkState<Ipv6>,
}

impl<I: Instant, D> IpLinkDeviceState<I, D> {
    /// Create a new `IpLinkDeviceState` with a link-specific state `link`.
    fn new(link: D) -> Self {
        Self {
            ip: IpDeviceState::default(),
            link,
            iplink_v4: IpLinkState::default(),
            iplink_v6: IpLinkState::default(),
        }
    }

    /// Get a reference to the inner (link-specific) state.
    fn link(&self) -> &D {
        &self.link
    }

    /// Get a mutable reference to the inner (link-specific) state.
    fn link_mut(&mut self) -> &mut D {
        &mut self.link
    }
}

impl<D: EventDispatcher> StateContext<IpDeviceState<D::Instant>, DeviceId> for Context<D> {
    fn get_state_with(&self, device: DeviceId) -> &IpDeviceState<D::Instant> {
        match device.protocol {
            DeviceProtocol::Ethernet => {
                &self.state().device.ethernet.get(device.id.into()).unwrap().device().ip
            }
        }
    }

    fn get_state_mut_with(&mut self, device: DeviceId) -> &mut IpDeviceState<D::Instant> {
        match device.protocol {
            DeviceProtocol::Ethernet => {
                &mut self
                    .state_mut()
                    .device
                    .ethernet
                    .get_mut(device.id.into())
                    .unwrap()
                    .device_mut()
                    .ip
            }
        }
    }
}

impl<D: EventDispatcher> StateContext<IpLinkState<Ipv4>, EthernetDeviceId> for Context<D> {
    fn get_state_with(&self, device: EthernetDeviceId) -> &IpLinkState<Ipv4> {
        &self.state().device.ethernet.get(device.0).unwrap().device().iplink_v4
    }

    fn get_state_mut_with(&mut self, device: EthernetDeviceId) -> &mut IpLinkState<Ipv4> {
        &mut self.state_mut().device.ethernet.get_mut(device.0).unwrap().device_mut().iplink_v4
    }
}

impl<D: EventDispatcher> StateContext<IpLinkState<Ipv6>, EthernetDeviceId> for Context<D> {
    fn get_state_with(&self, device: EthernetDeviceId) -> &IpLinkState<Ipv6> {
        &self.state().device.ethernet.get(device.0).unwrap().device().iplink_v6
    }

    fn get_state_mut_with(&mut self, device: EthernetDeviceId) -> &mut IpLinkState<Ipv6> {
        &mut self.state_mut().device.ethernet.get_mut(device.0).unwrap().device_mut().iplink_v6
    }
}

impl<D: EventDispatcher>
    StateContext<IpLinkDeviceState<D::Instant, EthernetDeviceState<D::Instant>>, EthernetDeviceId>
    for Context<D>
{
    fn get_state_with(
        &self,
        id: EthernetDeviceId,
    ) -> &IpLinkDeviceState<D::Instant, EthernetDeviceState<D::Instant>> {
        self.state().device.ethernet.get(id.0).unwrap().device()
    }

    fn get_state_mut_with(
        &mut self,
        id: EthernetDeviceId,
    ) -> &mut IpLinkDeviceState<D::Instant, EthernetDeviceState<D::Instant>> {
        self.state_mut().device.ethernet.get_mut(id.0).unwrap().device_mut()
    }
}

impl<D: EventDispatcher> StateContext<EthernetDeviceState<D::Instant>, EthernetDeviceId>
    for Context<D>
{
    fn get_state_with(&self, id: EthernetDeviceId) -> &EthernetDeviceState<D::Instant> {
        &self.state().device.ethernet.get(id.0).unwrap().device().link
    }

    fn get_state_mut_with(&mut self, id: EthernetDeviceId) -> &mut EthernetDeviceState<D::Instant> {
        &mut self.state_mut().device.ethernet.get_mut(id.0).unwrap().device_mut().link
    }
}

impl<D: EventDispatcher> StateContext<IpDeviceState<D::Instant>, EthernetDeviceId> for Context<D> {
    fn get_state_with(&self, id: EthernetDeviceId) -> &IpDeviceState<D::Instant> {
        &self.state().device.ethernet.get(id.0).unwrap().device().ip
    }

    fn get_state_mut_with(&mut self, id: EthernetDeviceId) -> &mut IpDeviceState<D::Instant> {
        &mut self.state_mut().device.ethernet.get_mut(id.0).unwrap().device_mut().ip
    }
}

impl<D: EventDispatcher> StateContext<IgmpInterface<D::Instant>, DeviceId> for Context<D> {
    fn get_state_with(&self, device: DeviceId) -> &IgmpInterface<D::Instant> {
        match device.protocol {
            DeviceProtocol::Ethernet => {
                &<Context<D> as StateContext<
                    IpLinkDeviceState<D::Instant, EthernetDeviceState<D::Instant>>,
                    EthernetDeviceId,
                >>::get_state_with(self, device.id().into())
                .ip
                .igmp
            }
        }
    }

    fn get_state_mut_with(&mut self, device: DeviceId) -> &mut IgmpInterface<D::Instant> {
        match device.protocol {
            DeviceProtocol::Ethernet => {
                &mut <Context<D> as StateContext<
                    IpLinkDeviceState<D::Instant, EthernetDeviceState<D::Instant>>,
                    EthernetDeviceId,
                >>::get_state_mut_with(self, device.id().into())
                .ip
                .igmp
            }
        }
    }
}

impl<D: EventDispatcher> StateContext<MldInterface<D::Instant>, DeviceId> for Context<D> {
    fn get_state_with(&self, device: DeviceId) -> &MldInterface<D::Instant> {
        match device.protocol {
            DeviceProtocol::Ethernet => {
                &<Context<D> as StateContext<
                    IpLinkDeviceState<D::Instant, EthernetDeviceState<D::Instant>>,
                    EthernetDeviceId,
                >>::get_state_with(self, device.id().into())
                .ip
                .mld
            }
        }
    }

    fn get_state_mut_with(&mut self, device: DeviceId) -> &mut MldInterface<D::Instant> {
        match device.protocol {
            DeviceProtocol::Ethernet => {
                &mut <Context<D> as StateContext<
                    IpLinkDeviceState<D::Instant, EthernetDeviceState<D::Instant>>,
                    EthernetDeviceId,
                >>::get_state_mut_with(self, device.id().into())
                .ip
                .mld
            }
        }
    }
}

impl<B: BufferMut, D: BufferDispatcher<B>> FrameContext<B, EthernetDeviceId> for Context<D> {
    fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        device: EthernetDeviceId,
        frame: S,
    ) -> Result<(), S> {
        self.dispatcher_mut().send_frame(device.into(), frame)
    }
}

impl Debug for EthernetDeviceId {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        let device: DeviceId = self.clone().into();
        write!(f, "{:?}", device)
    }
}

/// The identifier for timer events in the device layer.
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub(crate) struct DeviceLayerTimerId(DeviceLayerTimerIdInner);

#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
enum DeviceLayerTimerIdInner {
    /// A timer event for an Ethernet device.
    Ethernet(EthernetTimerId<EthernetDeviceId>),
}

impl From<EthernetTimerId<EthernetDeviceId>> for DeviceLayerTimerId {
    fn from(id: EthernetTimerId<EthernetDeviceId>) -> DeviceLayerTimerId {
        DeviceLayerTimerId(DeviceLayerTimerIdInner::Ethernet(id))
    }
}

impl From<EthernetDeviceId> for DeviceId {
    fn from(id: EthernetDeviceId) -> DeviceId {
        DeviceId::new_ethernet(id.0)
    }
}

impl<D: EventDispatcher> LinkDeviceIdContext<EthernetLinkDevice> for Context<D> {
    type DeviceId = EthernetDeviceId;
}

impl_timer_context!(
    DeviceLayerTimerId,
    EthernetTimerId<EthernetDeviceId>,
    DeviceLayerTimerId(DeviceLayerTimerIdInner::Ethernet(id)),
    id
);

/// Handle a timer event firing in the device layer.
pub(crate) fn handle_timeout<D: EventDispatcher>(ctx: &mut Context<D>, id: DeviceLayerTimerId) {
    match id.0 {
        DeviceLayerTimerIdInner::Ethernet(IpLinkDeviceTimerId::V4(id)) => {
            iplink::handle_timer::<Ipv4, EthernetLinkDevice, _>(ctx, id)
        }
        DeviceLayerTimerIdInner::Ethernet(IpLinkDeviceTimerId::V6(id)) => {
            iplink::handle_timer::<Ipv6, EthernetLinkDevice, _>(ctx, id)
        }
    }
}

/// An ID identifying a device.
#[derive(Copy, Clone, Eq, PartialEq, Hash)]
pub struct DeviceId {
    id: usize,
    protocol: DeviceProtocol,
}

// Temporary blanket impl until we switch over entirely to the traits defined in
// the `context` module.
impl<D: EventDispatcher> IpDeviceIdContext for Context<D> {
    type DeviceId = DeviceId;
}

impl From<usize> for DeviceId {
    fn from(id: usize) -> DeviceId {
        DeviceId::new_ethernet(id)
    }
}

impl From<DeviceId> for usize {
    fn from(id: DeviceId) -> usize {
        id.id
    }
}

impl DeviceId {
    /// Construct a new `DeviceId` for an Ethernet device.
    pub(crate) fn new_ethernet(id: usize) -> DeviceId {
        DeviceId { id, protocol: DeviceProtocol::Ethernet }
    }

    /// Get the protocol-specific ID for this `DeviceId`.
    pub fn id(self) -> usize {
        self.id
    }

    /// Get the protocol for this `DeviceId`.
    pub fn protocol(self) -> DeviceProtocol {
        self.protocol
    }
}

impl Display for DeviceId {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        write!(f, "{}:{}", self.protocol, self.id)
    }
}

impl Debug for DeviceId {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        Display::fmt(self, f)
    }
}

impl IdMapCollectionKey for DeviceId {
    const VARIANT_COUNT: usize = 1;

    fn get_variant(&self) -> usize {
        match self.protocol {
            DeviceProtocol::Ethernet => 0,
        }
    }

    fn get_id(&self) -> usize {
        self.id as usize
    }
}

/// Type of device protocol.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub enum DeviceProtocol {
    Ethernet,
}

impl Display for DeviceProtocol {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        write!(
            f,
            "{}",
            match self {
                DeviceProtocol::Ethernet => "Ethernet",
            }
        )
    }
}

// TODO(joshlf): Does the IP layer ever need to distinguish between broadcast
// and multicast frames?

/// The type of address used as the source address in a device-layer frame:
/// unicast or broadcast.
///
/// `FrameDestination` is used to implement RFC 1122 section 3.2.2 and RFC 4443
/// section 2.4.e, which govern when to avoid sending an ICMP error message for
/// ICMP and ICMPv6 respectively.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub(crate) enum FrameDestination {
    /// A unicast address - one which is neither multicast nor broadcast.
    Unicast,
    /// A multicast address; if the addressing scheme supports overlap between
    /// multicast and broadcast, then broadcast addresses should use the
    /// `Broadcast` variant.
    Multicast,
    /// A broadcast address; if the addressing scheme supports overlap between
    /// multicast and broadcast, then broadcast addresses should use the
    /// `Broadcast` variant.
    Broadcast,
}

impl FrameDestination {
    /// Is this `FrameDestination::Multicast`?
    pub(crate) fn is_multicast(self) -> bool {
        self == FrameDestination::Multicast
    }

    /// Is this `FrameDestination::Broadcast`?
    pub(crate) fn is_broadcast(self) -> bool {
        self == FrameDestination::Broadcast
    }
}

/// Builder for a [`DeviceLayerState`].
#[derive(Clone)]
pub struct DeviceStateBuilder {
    /// Default values for NDP's configurations for new interfaces.
    ///
    /// See [`ndp::NdpConfigurations`].
    default_ndp_configs: ndp::NdpConfigurations,
}

impl Default for DeviceStateBuilder {
    fn default() -> Self {
        Self { default_ndp_configs: ndp::NdpConfigurations::default() }
    }
}

impl DeviceStateBuilder {
    /// Set the default values for NDP's configurations for new interfaces.
    ///
    /// See [`ndp::NdpConfigurations`] for more details.
    pub fn set_default_ndp_configs(&mut self, v: ndp::NdpConfigurations) {
        self.default_ndp_configs = v;
    }

    /// Build the [`DeviceLayerState`].
    pub(crate) fn build<I: Instant>(self) -> DeviceLayerState<I> {
        DeviceLayerState { ethernet: IdMap::new(), default_ndp_configs: self.default_ndp_configs }
    }
}

/// The state associated with the device layer.
pub(crate) struct DeviceLayerState<I: Instant> {
    ethernet: IdMap<DeviceState<IpLinkDeviceState<I, EthernetDeviceState<I>>>>,
    default_ndp_configs: ndp::NdpConfigurations,
}

impl<I: Instant> DeviceLayerState<I> {
    /// Add a new ethernet device to the device layer.
    ///
    /// `add` adds a new `EthernetDeviceState` with the given MAC address and
    /// MTU. The MTU will be taken as a limit on the size of Ethernet payloads -
    /// the Ethernet header is not counted towards the MTU.
    pub(crate) fn add_ethernet_device(&mut self, mac: Mac, mtu: u32) -> DeviceId {
        let mut builder = EthernetDeviceStateBuilder::new(mac, mtu);
        builder.set_ndp_configs(self.default_ndp_configs.clone());
        let ethernet_state = DeviceState::new(IpLinkDeviceState::new(builder.build()));
        let id = self.ethernet.push(ethernet_state);
        debug!("adding Ethernet device with ID {} and MTU {}", id, mtu);
        DeviceId::new_ethernet(id)
    }
}

/// Initialization status of a device.
#[derive(Debug, PartialEq, Eq)]
enum InitializationStatus {
    /// The device is not yet initialized and MUST NOT be used.
    Uninitialized,

    /// The device is currently being initialized and must only be used by
    /// the initialization methods.
    Initializing,

    /// The device is initialized and can operate as normal.
    Initialized,
}

impl Default for InitializationStatus {
    #[inline]
    fn default() -> InitializationStatus {
        InitializationStatus::Uninitialized
    }
}

/// Common state across devices.
#[derive(Default)]
struct CommonDeviceState {
    /// The device's initialization status.
    initialization_status: InitializationStatus,
}

impl CommonDeviceState {
    fn is_initialized(&self) -> bool {
        self.initialization_status == InitializationStatus::Initialized
    }

    fn is_uninitialized(&self) -> bool {
        self.initialization_status == InitializationStatus::Uninitialized
    }
}

/// Device state.
///
/// `D` is the device-specific state.
struct DeviceState<D> {
    /// Device-independant state.
    common: CommonDeviceState,

    /// Device-specific state.
    device: D,
}

impl<D> DeviceState<D> {
    /// Create a new `DeviceState` with a device-specific state `device`.
    fn new(device: D) -> Self {
        Self { common: CommonDeviceState::default(), device }
    }

    /// Get a reference to the common (device-independant) state.
    fn common(&self) -> &CommonDeviceState {
        &self.common
    }

    /// Get a mutable reference to the common (device-independant) state.
    fn common_mut(&mut self) -> &mut CommonDeviceState {
        &mut self.common
    }

    /// Get a reference to the inner (device-specific) state.
    fn device(&self) -> &D {
        &self.device
    }

    /// Get a mutable reference to the inner (device-specific) state.
    fn device_mut(&mut self) -> &mut D {
        &mut self.device
    }
}

/// Generic IP-Device state.
// TODO(ghanan): Split this up into IPv4 and IPv6 specific device states.
struct IpDeviceState<I: Instant> {
    /// Assigned IPv4 addresses.
    // TODO(ghanan): Use `AddrSubnet` instead of `AddressEntry` as IPv4 addresses do not
    //               need the extra fields in `AddressEntry`.
    ipv4_addr_sub: Vec<AddressEntry<Ipv4Addr, I>>,

    /// Assigned IPv6 addresses.
    ///
    /// May be tentative (performing NDP's Duplicate Address Detection).
    ipv6_addr_sub: Vec<AddressEntry<Ipv6Addr, I>>,

    /// Assigned IPv6 link-local address.
    ///
    /// May be tentative (performing NDP's Duplicate Address Detection).
    ipv6_link_local_addr_sub: Option<AddressEntry<Ipv6Addr, I, LinkLocalAddr<Ipv6Addr>>>,

    /// IPv4 multicast groups this device has joined.
    ipv4_multicast_groups: HashMap<MulticastAddr<Ipv4Addr>, usize>,

    /// IPv6 multicast groups this device has joined.
    ipv6_multicast_groups: HashMap<MulticastAddr<Ipv6Addr>, usize>,

    /// Default hop limit for new IPv6 packets sent from this device.
    // TODO(ghanan): Once we separate out device-IP state from device-specific
    //               state, move this to some IPv6-device state.
    ipv6_hop_limit: NonZeroU8,

    /// A flag indicating whether routing of IPv4 packets not destined for this device is
    /// enabled.
    ///
    /// This flag controls whether or not packets can be routed from this device. That is, when a
    /// packet arrives at a device it is not destined for, the packet can only be routed if the
    /// device it arrived at has routing enabled and there exists another device that has a path
    /// to the packet's destination, regardless the other device's routing ability.
    ///
    /// Default: `false`.
    route_ipv4: bool,

    /// A flag indicating whether routing of IPv6 packets not destined for this device is
    /// enabled.
    ///
    /// This flag controls whether or not packets can be routed from this device. That is, when a
    /// packet arrives at a device it is not destined for, the packet can only be routed if the
    /// device it arrived at has routing enabled and there exists another device that has a path
    /// to the packet's destination, regardless the other device's routing ability.
    ///
    /// Default: `false`.
    route_ipv6: bool,

    /// MLD State.
    mld: MldInterface<I>,

    /// IGMP State.
    igmp: IgmpInterface<I>,
}

impl<I: Instant> Default for IpDeviceState<I> {
    fn default() -> IpDeviceState<I> {
        IpDeviceState {
            ipv4_addr_sub: Vec::new(),
            ipv6_addr_sub: Vec::new(),
            ipv6_link_local_addr_sub: None,
            ipv4_multicast_groups: HashMap::new(),
            ipv6_multicast_groups: HashMap::new(),
            ipv6_hop_limit: ndp::HOP_LIMIT_DEFAULT,
            route_ipv4: false,
            route_ipv6: false,
            mld: MldInterface::default(),
            igmp: IgmpInterface::default(),
        }
    }
}

/// The various states an IP address can be on an interface.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AddressState {
    /// The address is assigned to an interface and can be considered
    /// bound to it (all packets destined to the address will be
    /// accepted).
    Assigned,

    /// The address is considered unassigned to an interface for normal
    /// operations, but has the intention of being assigned in the future
    /// (e.g. once NDP's Duplicate Address Detection is completed).
    Tentative,

    /// The address is considered deprecated on an interface. Existing
    /// connections using the address will be fine, however new connections
    /// should not use the deprecated address.
    Deprecated,
}

impl AddressState {
    /// Is this address assigned?
    fn is_assigned(self) -> bool {
        self == AddressState::Assigned
    }

    /// Is this address tentative?
    fn is_tentative(self) -> bool {
        self == AddressState::Tentative
    }
}

/// The type of address configuraion.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AddressConfigurationType {
    /// Configured by stateless address autoconfiguration.
    Slaac,

    /// Manually configured.
    Manual,
}

/// Data associated with an IP addressess on an interface.
pub struct AddressEntry<S: IpAddress, T: Instant, A: Witness<S> = SpecifiedAddr<S>> {
    addr_sub: AddrSubnet<S, A>,
    state: AddressState,
    configuration_type: AddressConfigurationType,
    valid_until: Option<T>,
}

impl<S: IpAddress, T: Instant, A: Witness<S>> AddressEntry<S, T, A> {
    fn new(
        addr_sub: AddrSubnet<S, A>,
        state: AddressState,
        configuration_type: AddressConfigurationType,
        valid_until: Option<T>,
    ) -> Self {
        Self { addr_sub, state, configuration_type, valid_until }
    }

    fn addr_sub(&self) -> &AddrSubnet<S, A> {
        &self.addr_sub
    }

    fn state(&self) -> AddressState {
        self.state
    }

    fn configuration_type(&self) -> AddressConfigurationType {
        self.configuration_type
    }

    fn valid_until(&self) -> Option<T> {
        self.valid_until
    }

    fn mark_permanent(&mut self) {
        self.state = AddressState::Assigned;
    }
}

/// Possible return values during an erroneous interface address change operation.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum AddressError {
    AlreadyExists,
    NotFound,
}

/// An event dispatcher for the device layer.
///
/// See the `EventDispatcher` trait in the crate root for more details.
pub trait DeviceLayerEventDispatcher<B: BufferMut> {
    /// Send a frame to a device driver.
    ///
    /// If there was an MTU error while attempting to serialize the frame, the
    /// original serializer is returned in the `Err` variant. All other errors
    /// (for example, errors in allocating a buffer) are silently ignored and
    /// reported as success.
    ///
    /// Note, until `device` has been initialized, the netstack promises to not
    /// send any outbound traffic to it. See [`initialize_device`] for more
    /// information.
    fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        device: DeviceId,
        frame: S,
    ) -> Result<(), S>;
}

/// Is `device` usable?
///
/// That is, is it either initializing or initialized?
pub(crate) fn is_device_usable<D: EventDispatcher>(
    state: &StackState<D>,
    device: DeviceId,
) -> bool {
    !get_common_device_state(state, device).is_uninitialized()
}

/// Is `device` initialized?
pub(crate) fn is_device_initialized<D: EventDispatcher>(
    state: &StackState<D>,
    device: DeviceId,
) -> bool {
    get_common_device_state(state, device).is_initialized()
}

/// Initialize a device.
///
/// `initialize_device` will start soliciting IPv6 routers on the link if `device` is configured to
/// be a host. If it is configured to be an advertising interface, it will start sending periodic
/// router advertisements.
///
/// `initialize_device` MUST be called after adding the device to the netstack. A device MUST NOT
/// be used until it has been initialized.
///
/// This initialize step is kept separated from the device creation/allocation step so that
/// implementations have a chance to do some work (such as updating implementation specific IDs or
/// state, configure the device or driver, etc.) before the device is actually initialized and used
/// by this netstack.
///
/// See [`StackState::add_ethernet_device`] for information about adding ethernet devices.
///
/// # Panics
///
/// Panics if `device` is already initialized.
pub fn initialize_device<D: EventDispatcher>(ctx: &mut Context<D>, device: DeviceId) {
    let state = get_common_device_state_mut(ctx.state_mut(), device);

    // `device` must currently be uninitialized.
    assert!(state.is_uninitialized());

    state.initialization_status = InitializationStatus::Initializing;

    <Context<D> as IpDeviceHandler<Ipv4>>::initialize_device(ctx, device);
    <Context<D> as IpDeviceHandler<Ipv6>>::initialize_device(ctx, device);

    get_common_device_state_mut(ctx.state_mut(), device).initialization_status =
        InitializationStatus::Initialized;
}

/// Remove a device from the device layer.
///
/// This function returns frames for the bindings to send if the shutdown is graceful - they can be
/// safely ignored otherwise.
///
/// # Panics
///
/// Panics if `device` does not refer to an existing device.
pub fn remove_device<D: EventDispatcher>(
    ctx: &mut Context<D>,
    device: DeviceId,
) -> Option<Vec<usize>> {
    match device.protocol {
        DeviceProtocol::Ethernet => {
            // TODO(rheacock): Generate any final frames to send here.
            <Context<D> as IpDeviceHandler<Ipv4>>::deinitialize_device(ctx, device);
            <Context<D> as IpDeviceHandler<Ipv6>>::deinitialize_device(ctx, device);
            ctx.state_mut()
                .device
                .ethernet
                .remove(device.id)
                .unwrap_or_else(|| panic!("no such Ethernet device: {}", device.id));
            debug!("removing Ethernet device with ID {}", device.id);
            None
        }
    }
}

/// Send an IP packet in a device layer frame.
///
/// `send_ip_frame` accepts a device ID, a local IP address, and a
/// `SerializationRequest`. It computes the routing information and serializes
/// the request in a new device layer frame and sends it.
///
/// # Panics
///
/// Panics if `device` is not initialized.
#[specialize_ip_address]
pub(crate) fn send_ip_frame<
    B: BufferMut,
    D: BufferDispatcher<B>,
    A: IpAddress,
    S: Serializer<Buffer = B>,
>(
    ctx: &mut Context<D>,
    device: DeviceId,
    local_addr: SpecifiedAddr<A>,
    body: S,
) -> Result<(), S> {
    // `device` must not be uninitialized.
    assert!(is_device_usable(ctx.state(), device));

    match device.protocol {
        DeviceProtocol::Ethernet => {
            return ctx.send_frame(IpLinkFrameMeta::new(device.id.into(), local_addr), body)
        }
    }
}

/// Receive a device layer frame from the network.
///
/// # Panics
///
/// Panics if `device` is not initialized.
pub fn receive_frame<B: BufferMut, D: BufferDispatcher<B>>(
    ctx: &mut Context<D>,
    device: DeviceId,
    buffer: B,
) {
    // `device` must be initialized.
    assert!(is_device_initialized(ctx.state(), device));

    match device.protocol {
        DeviceProtocol::Ethernet => self::ethernet::receive_frame(ctx, device.id.into(), buffer),
    }
}

/// Set the promiscuous mode flag on `device`.
// TODO(rheacock): remove `allow(dead_code)` when this is used.
#[allow(dead_code)]
pub(crate) fn set_promiscuous_mode<D: EventDispatcher>(
    ctx: &mut Context<D>,
    device: DeviceId,
    enabled: bool,
) {
    match device.protocol {
        DeviceProtocol::Ethernet => {
            self::ethernet::set_promiscuous_mode(ctx, device.id.into(), enabled)
        }
    }
}

/// Get a single IP address and subnet for a device.
///
/// Note, tentative IP addresses (addresses which are not yet fully bound to a
/// device) will not be returned by `get_ip_addr`.
pub(crate) fn get_ip_addr_subnet<D: EventDispatcher, A: IpAddress>(
    ctx: &Context<D>,
    device: DeviceId,
) -> Option<AddrSubnet<A>> {
    <Context<D> as IpDeviceHandler<A::Version>>::get_ip_addr_subnet(ctx, device)
}

/// Get the IP addresses and associated subnets for a device.
///
/// Note, tentative IP addresses (addresses which are not yet fully bound to a
/// device) will not be returned by `get_ip_addr_subnets`.
///
/// Returns an [`Iterator`] of `AddrSubnet<A>`.
///
/// See [`Tentative`] and [`AddrSubnet`] for more information.
pub fn get_ip_addr_subnets<'a, D: EventDispatcher, A: IpAddress>(
    ctx: &'a Context<D>,
    device: DeviceId,
) -> impl 'a + Iterator<Item = AddrSubnet<A>> {
    <Context<D> as IpDeviceHandler<A::Version>>::get_ip_addr_subnets(ctx, device)
}

/// Get the state of an address on device.
///
/// Returns `None` if `addr` is not associated with `device`.
pub(crate) fn get_ip_addr_state<D: EventDispatcher, A: IpAddress>(
    ctx: &Context<D>,
    device: DeviceId,
    addr: &SpecifiedAddr<A>,
) -> Option<AddressState> {
    <Context<D> as IpDeviceHandler<A::Version>>::get_ip_addr_state(ctx, device, addr)
}

/// Checks if `addr` is a local address
pub(crate) fn is_local_addr<D: EventDispatcher, A: IpAddress>(
    ctx: &Context<D>,
    addr: &SpecifiedAddr<A>,
) -> bool {
    // TODO(brunodalbo) this is a total hack just to enable UDP sockets in
    // bindings.
    let device_ids: Vec<_> = ctx.state.device.ethernet.iter().map(|(k, _)| k).collect();
    device_ids.into_iter().any(|id| {
        <Context<D> as IpDeviceHandler<A::Version>>::get_ip_addr_state(ctx, id.into(), addr)
            .map(|s| s.is_assigned())
            .unwrap_or(false)
    })
}

/// Adds an IP address and associated subnet to this device.
///
/// # Panics
///
/// Panics if `device` is not initialized.
#[specialize_ip_address]
pub fn add_ip_addr_subnet<D: EventDispatcher, A: IpAddress>(
    ctx: &mut Context<D>,
    device: DeviceId,
    addr_sub: AddrSubnet<A>,
) -> Result<(), AddressError> {
    // `device` must be initialized.
    assert!(is_device_initialized(ctx.state(), device));

    trace!("add_ip_addr_subnet: adding addr {:?} to device {:?}", addr_sub, device);

    let res =
        <Context<D> as IpDeviceHandler<A::Version>>::add_ip_addr_subnet(ctx, device, addr_sub);

    if res.is_ok() {
        #[ipv4addr]
        crate::ip::socket::apply_ipv4_socket_update(ctx, IpSockUpdate::new());
        #[ipv6addr]
        crate::ip::socket::apply_ipv6_socket_update(ctx, IpSockUpdate::new());
    }

    res
}

/// Removes an IP address and associated subnet to this device.
///
/// # Panics
///
/// Panics if `device` is not initialized.
#[specialize_ip_address]
pub fn del_ip_addr<D: EventDispatcher, A: IpAddress>(
    ctx: &mut Context<D>,
    device: DeviceId,
    addr: &SpecifiedAddr<A>,
) -> Result<(), AddressError> {
    // `device` must be initialized.
    assert!(is_device_initialized(ctx.state(), device));

    trace!("del_ip_addr: removing addr {:?} from device {:?}", addr, device);

    let res = <Context<D> as IpDeviceHandler<A::Version>>::del_ip_addr(ctx, device, addr);

    if res.is_ok() {
        #[ipv4addr]
        crate::ip::socket::apply_ipv4_socket_update(ctx, IpSockUpdate::new());
        #[ipv6addr]
        crate::ip::socket::apply_ipv6_socket_update(ctx, IpSockUpdate::new());
    }

    res
}

/// Add `device` to a multicast group `multicast_addr`.
///
/// Calling `join_ip_multicast` with the same `device` and `multicast_addr` is completely safe.
/// A counter will be kept for the number of times `join_ip_multicast` has been called with the
/// same `device` and `multicast_addr` pair. To completely leave a multicast group,
/// [`leave_ip_multicast`] must be called the same number of times `join_ip_multicast` has been
/// called for the same `device` and `multicast_addr` pair. The first time `join_ip_multicast` is
/// called for a new `device` and `multicast_addr` pair, the device will actually join the multicast
/// group.
///
/// # Panics
///
/// Panics if `device` is not initialized.
#[cfg(test)]
pub(crate) fn join_ip_multicast<D: EventDispatcher, A: IpAddress>(
    ctx: &mut Context<D>,
    device: DeviceId,
    multicast_addr: MulticastAddr<A>,
) {
    // `device` must not be uninitialized.
    assert!(is_device_usable(ctx.state(), device));

    trace!("join_ip_multicast: device {:?} joining multicast {:?}", device, multicast_addr);

    <Context<D> as IpDeviceHandler<A::Version>>::join_ip_multicast(ctx, device, multicast_addr)
}

/// Attempt to remove `device` from a multicast group `multicast_addr`.
///
/// `leave_ip_multicast` will attempt to remove `device` from a multicast group `multicast_addr`.
/// `device` may have "joined" the same multicast address multiple times, so `device` will only
/// leave the multicast group once `leave_ip_multicast` has been called for each corresponding
/// [`join_ip_multicast`]. That is, if `join_ip_multicast` gets called 3 times and
/// `leave_ip_multicast` gets called two times (after all 3 `join_ip_multicast` calls), `device`
/// will still be in the multicast group until the next (final) call to `leave_ip_multicast`.
///
/// # Panics
///
/// Panics if `device` is not initialized or `device` is not currently in the multicast group.
// TODO(ghanan): remove when this is called from non-test code
#[cfg(test)]
pub(crate) fn leave_ip_multicast<D: EventDispatcher, A: IpAddress>(
    ctx: &mut Context<D>,
    device: DeviceId,
    multicast_addr: MulticastAddr<A>,
) {
    // `device` must not be uninitialized.
    assert!(is_device_usable(ctx.state(), device));

    trace!("join_ip_multicast: device {:?} leaving multicast {:?}", device, multicast_addr);

    <Context<D> as IpDeviceHandler<A::Version>>::leave_ip_multicast(ctx, device, multicast_addr)
}

/// Is `device` part of the IP multicast group `multicast_addr`.
pub(crate) fn is_in_ip_multicast<D: EventDispatcher, A: IpAddress>(
    ctx: &Context<D>,
    device: DeviceId,
    multicast_addr: MulticastAddr<A>,
) -> bool {
    <Context<D> as IpDeviceHandler<A::Version>>::is_in_ip_multicast(ctx, device, multicast_addr)
}

/// Get the MTU associated with this device.
pub(crate) fn get_mtu<D: EventDispatcher>(ctx: &Context<D>, device: DeviceId) -> u32 {
    <Context<D> as IpDeviceHandler<Ipv6>>::get_mtu(ctx, device)
}

/// Get the hop limit for new IPv6 packets that will be sent out from `device`.
pub(crate) fn get_ipv6_hop_limit<D: EventDispatcher>(
    ctx: &Context<D>,
    device: DeviceId,
) -> NonZeroU8 {
    <Context<D> as IpDeviceHandler<Ipv6>>::get_hop_limit(ctx, device)
}

/// Gets the IPv6 link-local address associated with this device.
// TODO(brunodalbo) when our device model allows for multiple IPs we can have
// a single function go get all the IP addresses associated with a device, which
// would be cleaner and remove the need for this function.
pub fn get_ipv6_link_local_addr<D: EventDispatcher>(
    ctx: &Context<D>,
    device: DeviceId,
) -> Option<LinkLocalAddr<Ipv6Addr>> {
    <Context<D> as IpDeviceHandler<Ipv6>>::get_link_local_addr(ctx, device.id.into())
}

/// Determine if an IP Address is considered tentative on a device.
///
/// Returns `true` if the address is tentative on a device; `false` otherwise.
/// Note, if the `addr` is not assigned to `device` but is considered tentative
/// on another device, `is_addr_tentative_on_device` will return `false`.
pub(crate) fn is_addr_tentative_on_device<D: EventDispatcher, A: IpAddress>(
    ctx: &Context<D>,
    addr: &SpecifiedAddr<A>,
    device: DeviceId,
) -> bool {
    get_ip_addr_state::<_, A>(ctx, device, addr).map_or(false, |x| x.is_tentative())
}

/// Get a reference to the common device state for a `device`.
fn get_common_device_state<D: EventDispatcher>(
    state: &StackState<D>,
    device: DeviceId,
) -> &CommonDeviceState {
    match device.protocol {
        DeviceProtocol::Ethernet => state
            .device
            .ethernet
            .get(device.id)
            .unwrap_or_else(|| panic!("no such Ethernet device: {}", device.id))
            .common(),
    }
}

/// Get a mutable reference to the common device state for a `device`.
fn get_common_device_state_mut<D: EventDispatcher>(
    state: &mut StackState<D>,
    device: DeviceId,
) -> &mut CommonDeviceState {
    match device.protocol {
        DeviceProtocol::Ethernet => state
            .device
            .ethernet
            .get_mut(device.id)
            .unwrap_or_else(|| panic!("no such Ethernet device: {}", device.id))
            .common_mut(),
    }
}

/// Is IP packet routing enabled on `device`?
///
/// Note, `true` does not necessarily mean that `device` is currently routing IP packets. It
/// only means that `device` is allowed to route packets. To route packets, this netstack must
/// be configured to allow IP packets to be routed if it was not destined for this node.
pub(crate) fn is_routing_enabled<D: EventDispatcher, I: Ip>(
    ctx: &Context<D>,
    device: DeviceId,
) -> bool {
    <Context<D> as IpDeviceHandler<I>>::is_routing_enabled(ctx, device.id.into())
}

/// Enables or disables IP packet routing on `device`.
///
/// `set_routing_enabled` does nothing if the new routing status, `enabled`, is the same as
/// the current routing status.
///
/// Note, enabling routing does not mean that `device` will immediately start routing IP
/// packets. It only means that `device` is allowed to route packets. To route packets, this
/// netstack must be configured to allow IP packets to be routed if it was not destined for this
/// node.
#[specialize_ip]
pub(crate) fn set_routing_enabled<D: EventDispatcher, I: Ip>(
    ctx: &mut Context<D>,
    device: DeviceId,
    enabled: bool,
) {
    <Context<D> as IpDeviceHandler<I>>::set_routing_enabled(ctx, device, enabled)
}

/// Insert a static entry into this device's ARP table.
///
/// This will cause any conflicting dynamic entry to be removed, and
/// any future conflicting gratuitous ARPs to be ignored.
// TODO(rheacock): remove `cfg(test)` when this is used. Will probably be
// called by a pub fn in the device mod.
#[cfg(test)]
pub(super) fn insert_static_arp_table_entry<D: EventDispatcher>(
    ctx: &mut Context<D>,
    device: DeviceId,
    addr: Ipv4Addr,
    mac: Mac,
) {
    match device.protocol {
        DeviceProtocol::Ethernet => iplink::insert_static_neighbor::<Ipv4, EthernetLinkDevice, _>(
            ctx,
            device.id.into(),
            addr,
            mac,
        ),
    }
}

/// Insert an entry into this device's NDP table.
///
/// This method only gets called when testing to force set a neighbor's
/// link address so that lookups succeed immediately, without doing
/// address resolution.
// TODO(rheacock): remove when this is called from non-test code
#[cfg(test)]
pub(crate) fn insert_ndp_table_entry<D: EventDispatcher>(
    ctx: &mut Context<D>,
    device: DeviceId,
    addr: Ipv6Addr,
    mac: Mac,
) {
    match device.protocol {
        DeviceProtocol::Ethernet => iplink::insert_static_neighbor::<Ipv6, EthernetLinkDevice, _>(
            ctx,
            device.id.into(),
            addr,
            mac,
        ),
    }
}

/// Updates the NDP Configurations for a `device`.
///
/// Note, some values may not take effect immediately, and may only take effect the next time they
/// are used. These scenarios documented below:
///
///  - Updates to [`NdpConfiguration::dup_addr_detect_transmits`] will only take effect the next
///    time Duplicate Address Detection (DAD) is done. Any DAD processes that have already started
///    will continue using the old value.
///
///  - Updates to [`NdpConfiguration::max_router_solicitations`] will only take effect the next
///    time routers are explicitly solicited. Current router solicitation will continue using the
///    old value.
// TODO(rheacock): remove `allow(dead_code)` when this is used.
#[allow(dead_code)]
pub fn set_ndp_configurations<D: EventDispatcher>(
    ctx: &mut Context<D>,
    device: DeviceId,
    configs: ndp::NdpConfigurations,
) {
    match device.protocol {
        DeviceProtocol::Ethernet => {
            <Context<_> as NdpHandler<EthernetLinkDevice>>::set_configurations(
                ctx,
                device.id.into(),
                configs,
            )
        }
    }
}

/// Gets the NDP Configurations for a `device`.
// TODO(rheacock): remove `allow(dead_code)` when this is used.
#[allow(dead_code)]
pub fn get_ndp_configurations<D: EventDispatcher>(
    ctx: &Context<D>,
    device: DeviceId,
) -> &ndp::NdpConfigurations {
    match device.protocol {
        DeviceProtocol::Ethernet => {
            <Context<_> as NdpHandler<EthernetLinkDevice>>::get_configurations(
                ctx,
                device.id.into(),
            )
        }
    }
}

/// An address that may be "tentative" in that it has not yet passed
/// duplicate address detection (DAD).
///
/// A tentative address is one for which DAD is currently being performed.
/// An address is only considered assigned to an interface once DAD has
/// completed without detecting any duplicates. See [RFC 4862] for more details.
///
/// [RFC 4862]: https://tools.ietf.org/html/rfc4862
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Tentative<T>(T, bool);

impl<T> Tentative<T> {
    /// Create a new address that is marked as tentative.
    pub(crate) fn new_tentative(t: T) -> Self {
        Self(t, true)
    }

    /// Create a new address that is marked as permanent/assigned.
    pub(crate) fn new_permanent(t: T) -> Self {
        Self(t, false)
    }

    /// Returns whether the value is tentative.
    pub(crate) fn is_tentative(&self) -> bool {
        self.1
    }

    /// Gets the value that is stored inside.
    pub(crate) fn into_inner(self) -> T {
        self.0
    }

    /// Converts a `Tentative<T>` into a `Option<T>` in the way that
    /// a tentative value corresponds to a `None`.
    pub(crate) fn try_into_permanent(self) -> Option<T> {
        if self.is_tentative() {
            None
        } else {
            Some(self.into_inner())
        }
    }
}

impl<D: EventDispatcher> NdpPacketHandler<DeviceId> for Context<D> {
    fn receive_ndp_packet<B: ByteSlice>(
        &mut self,
        device: DeviceId,
        src_ip: Ipv6Addr,
        dst_ip: SpecifiedAddr<Ipv6Addr>,
        packet: NdpPacket<B>,
    ) {
        trace!("device::receive_ndp_packet");

        match device.protocol {
            DeviceProtocol::Ethernet => {
                receive_ndp_packet(self, device.id.into(), src_ip, dst_ip, packet);
            }
        }
    }
}
