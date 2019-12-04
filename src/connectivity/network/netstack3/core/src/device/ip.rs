// Copyright 2019 The Fuchsia Authors. All rights reAserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::fmt::{Debug, Display};
use std::iter::FilterMap;
use std::num::NonZeroU8;
use std::slice::Iter;

use log::trace;
use net_types::ip::{AddrSubnet, Ip, IpAddress, Ipv6};
use net_types::{LinkLocalAddr, LinkLocalAddress, MulticastAddr, SpecifiedAddr, Witness};
use specialize_ip_macro::{specialize_ip, specialize_ip_address};

use crate::context::{InstantContext, StateContext};
use crate::device::ethernet::{BufferEthernetIpLinkDeviceHandler, EthernetLinkDevice};
use crate::device::iplink::IpLinkDeviceHandler;
use crate::device::link::LinkDeviceHandler;
use crate::device::{
    AddressConfigurationType, AddressEntry, AddressError, AddressState, DeviceProtocol,
    IpDeviceState, Tentative,
};
use crate::ip::IpHandler;
use crate::{Context, EventDispatcher};

/// An execution context which provides a `DeviceId` type for various IP
/// internals to share.
///
/// This trait provides the associated `DeviceId` type, and is used by
/// [`IgmpContext`], [`MldContext`], and [`IcmpContext`]. It allows them to use
/// the same `DeviceId` type rather than each providing their own, which would
/// require lots of verbose type bounds when they need to be interoperable (such
/// as when ICMP delivers an MLD packet to the `mld` module for processing).
pub(crate) trait IpDeviceIdContext {
    type DeviceId: Copy + Display + Debug + Send + Sync + 'static;
}

/// The context provided by the device layer to an IP device implementation.
pub(super) trait IpDeviceContext<I: Ip>:
    IpDeviceIdContext
    + InstantContext
    + StateContext<
        IpDeviceState<<Self as InstantContext>::Instant>,
        <Self as IpDeviceIdContext>::DeviceId,
    > + IpHandler
    + BufferEthernetIpLinkDeviceHandler<I, packet::EmptyBuf>
{
    fn get_device_protocol_id(
        device: <Self as IpDeviceIdContext>::DeviceId,
    ) -> (DeviceProtocol, usize);
}

impl<I: Ip, D: EventDispatcher> IpDeviceContext<I> for Context<D> {
    fn get_device_protocol_id(
        device: <Self as IpDeviceIdContext>::DeviceId,
    ) -> (DeviceProtocol, usize) {
        (device.protocol, device.id)
    }
}

pub(super) trait IpDeviceHandlerPrivate<I: Ip>: IpDeviceIdContext + InstantContext {
    /// Get the state of an address on a device.
    ///
    /// If `configuration_type` is provided, then only the state of an address of that
    /// configuration type will be returned.
    ///
    /// Returns `None` if `addr` is not associated with `device_id`.
    // TODO(ghanan): Use `SpecializedAddr` for `addr`.
    fn get_ip_addr_state_inner(
        &mut self,
        device_id: Self::DeviceId,
        addr: &I::Addr,
        configuration_type: Option<AddressConfigurationType>,
    ) -> Option<AddressState>;

    /// Adds an IP address and associated subnet to this device.
    ///
    /// `configuration_type` is the way this address is being configured.
    /// See [`AddressConfigurationType`] for more details.
    ///
    /// # Panics
    ///
    /// Panics if `addr_sub` holds a link-local address.
    fn add_ip_addr_subnet_inner(
        &mut self,
        device_id: Self::DeviceId,
        addr_sub: AddrSubnet<I::Addr>,
        configuration_type: AddressConfigurationType,
        valid_until: Option<Self::Instant>,
    ) -> Result<(), AddressError>;

    /// Removes an IP address and associated subnet from this device.
    ///
    /// If `configuration_type` is provided, then only an address of that
    /// configuration type will be removed.
    ///
    /// # Panics
    ///
    /// Panics if `addr` is a link-local address.
    fn del_ip_addr_inner(
        &mut self,
        device_id: Self::DeviceId,
        addr: &I::Addr,
        configuration_type: Option<AddressConfigurationType>,
    ) -> Result<(), AddressError>;

    fn set_routing_enabled_inner(&mut self, device_id: Self::DeviceId, enabled: bool);

    fn initialize_device_inner(&mut self, device: Self::DeviceId);
}

impl<I: Ip, C: IpDeviceContext<I>> IpDeviceHandlerPrivate<I> for C {
    fn get_ip_addr_state_inner(
        &mut self,
        device_id: Self::DeviceId,
        addr: &I::Addr,
        configuration_type: Option<AddressConfigurationType>,
    ) -> Option<AddressState> {
        get_ip_addr_state_inner::<_, I::Addr>(self, device_id, addr, configuration_type)
    }

    fn add_ip_addr_subnet_inner(
        &mut self,
        device_id: Self::DeviceId,
        addr_sub: AddrSubnet<I::Addr>,
        configuration_type: AddressConfigurationType,
        valid_until: Option<Self::Instant>,
    ) -> Result<(), AddressError> {
        add_ip_addr_subnet_inner::<_, I::Addr>(
            self,
            device_id,
            addr_sub,
            configuration_type,
            valid_until,
        )
    }

    fn del_ip_addr_inner(
        &mut self,
        device_id: Self::DeviceId,
        addr: &I::Addr,
        configuration_type: Option<AddressConfigurationType>,
    ) -> Result<(), AddressError> {
        del_ip_addr_inner::<_, I::Addr>(self, device_id, addr, configuration_type)
    }

    fn set_routing_enabled_inner(&mut self, device_id: Self::DeviceId, enabled: bool) {
        set_routing_enabled_inner::<_, I>(self, device_id, enabled)
    }

    fn initialize_device_inner(&mut self, device: Self::DeviceId) {
        initialize_device_inner(self, device)
    }
}

pub(super) trait IpDeviceHandler<I: Ip>: IpDeviceIdContext + InstantContext {
    /// Initialize a device.
    ///
    /// `initialize_device` sets the link-local address for `device_id` and performs DAD on it.
    ///
    /// `device_id` MUST be ready to send packets before `initialize_device` is called.
    fn initialize_device(&mut self, device: Self::DeviceId);

    /// Remove a device from the device layer.
    ///
    /// This function returns frames for the bindings to send if the shutdown is graceful - they can be
    /// safely ignored otherwise.
    ///
    /// # Panics
    ///
    /// Panics if `device` does not refer to an existing device.
    fn deinitialize_device(&mut self, device: Self::DeviceId);

    /// Get a single IP address and subnet for a device.
    ///
    /// Note, tentative IP addresses (addresses which are not yet fully bound to a
    /// device) will not be returned by `get_ip_addr`.
    fn get_ip_addr_subnet(&self, device: Self::DeviceId) -> Option<AddrSubnet<I::Addr>>;

    /// Get the IP addresses and associated subnets for a device.
    ///
    /// Note, tentative IP addresses (addresses which are not yet fully bound to a
    /// device) will not be returned by `get_ip_addr_subnets`.
    ///
    /// Returns an [`Iterator`] of `AddrSubnet<A>`.
    ///
    /// See [`Tentative`] and [`AddrSubnet`] for more information.
    fn get_ip_addr_subnets(
        &self,
        device: Self::DeviceId,
    ) -> FilterMap<
        Iter<AddressEntry<I::Addr, Self::Instant>>,
        fn(&AddressEntry<I::Addr, Self::Instant>) -> Option<AddrSubnet<I::Addr>>,
    >;

    /// Get the IP address and subnet associated with this device, including tentative
    /// addresses.
    ///
    /// Note, deprecated IP addresses (addresses which have been assigned but should no
    /// longer be used for new connections) will not be returned by
    /// `get_ip_addr_subnets_with_tentative`.
    ///
    /// Returns an [`Iterator`] of `Tentative<AddrSubnet<A>>`.
    ///
    /// See [`Tentative`] and [`AddrSubnet`] for more information.
    fn get_ip_addr_subnets_with_tentative(
        &self,
        device: Self::DeviceId,
    ) -> FilterMap<
        Iter<AddressEntry<I::Addr, Self::Instant>>,
        fn(&AddressEntry<I::Addr, Self::Instant>) -> Option<Tentative<AddrSubnet<I::Addr>>>,
    >;

    /// Get the state of an address on a device.
    ///
    /// Returns `None` if `addr` is not associated with `device_id`.
    fn get_ip_addr_state(
        &self,
        device: Self::DeviceId,
        addr: &SpecifiedAddr<I::Addr>,
    ) -> Option<AddressState>;

    /// Adds an IP address and associated subnet to this device.
    ///
    /// # Panics
    ///
    /// Panics if `addr_sub` holds a link-local address.
    // TODO(ghanan): Use a witness type to guarantee non-link-local-ness for `addr_sub`.
    fn add_ip_addr_subnet(
        &mut self,
        device: Self::DeviceId,
        addr_sub: AddrSubnet<I::Addr>,
    ) -> Result<(), AddressError>;

    /// Removes an IP address and associated subnet from this device.
    ///
    /// # Panics
    ///
    /// Panics if `addr` is a link-local address.
    // TODO(ghanan): Use a witness type to guarantee non-link-local-ness for `addr`.
    fn del_ip_addr(
        &mut self,
        device: Self::DeviceId,
        addr: &SpecifiedAddr<I::Addr>,
    ) -> Result<(), AddressError>;

    /// Get the link-local address associated with this device.
    ///
    /// Returns `None` if the address is tentative.
    fn get_link_local_addr(&self, device: Self::DeviceId) -> Option<LinkLocalAddr<I::Addr>>;

    /// Add `device_id` to a multicast group `multicast_addr`.
    ///
    /// Calling `join_ip_multicast` with the same `device_id` and `multicast_addr` is completely safe.
    /// A counter will be kept for the number of times `join_ip_multicast` has been called with the
    /// same `device_id` and `multicast_addr` pair. To completely leave a multicast group,
    /// [`leave_ip_multicast`] must be called the same number of times `join_ip_multicast` has been
    /// called for the same `device_id` and `multicast_addr` pair. The first time `join_ip_multicast` is
    /// called for a new `device` and `multicast_addr` pair, the device will actually join the multicast
    /// group.
    ///
    /// `join_ip_multicast` is different from [`join_link_multicast`] as `join_ip_multicast` joins an
    /// L3 multicast group, whereas `join_link_multicast` joins an L2 multicast group.
    fn join_ip_multicast(&mut self, device: Self::DeviceId, multicast_addr: MulticastAddr<I::Addr>);

    /// Remove `device_id` from a multicast group `multicast_addr`.
    ///
    /// `leave_ip_multicast` will attempt to remove `device_id` from a multicast group `multicast_addr`.
    /// `device_id` may have "joined" the same multicast address multiple times, so `device_id` will
    /// only leave the multicast group once `leave_ip_multicast` has been called for each corresponding
    /// [`join_ip_multicast`]. That is, if `join_ip_multicast` gets called 3 times and
    /// `leave_ip_multicast` gets called two times (after all 3 `join_ip_multicast` calls), `device_id`
    /// will still be in the multicast group until the next (final) call to `leave_ip_multicast`.
    ///
    /// `leave_ip_multicast` is different from [`leave_link_multicast`] as `leave_ip_multicast` leaves
    /// an L3 multicast group, whereas `leave_link_multicast` leaves an L2 multicast group.
    ///
    /// # Panics
    ///
    /// If `device_id` is not currently in the multicast group `multicast_addr`.
    fn leave_ip_multicast(
        &mut self,
        device: Self::DeviceId,
        multicast_addr: MulticastAddr<I::Addr>,
    );

    /// Is `device` in the IP multicast group `multicast_addr`?
    fn is_in_ip_multicast(
        &self,
        device: Self::DeviceId,
        multicast_addr: MulticastAddr<I::Addr>,
    ) -> bool;

    /// Get the MTU associated with this device.
    fn get_mtu(&self, device: Self::DeviceId) -> u32;

    /// Get the hop limit for new IP packets that will be sent out from `device`.
    fn get_hop_limit(&self, device: Self::DeviceId) -> NonZeroU8;

    /// Is IP packet routing enabled on `device_id`?
    ///
    /// Note, `true` does not necessarily mean that `device` is currently routing IP packets. It
    /// only means that `device` is allowed to route packets. To route packets, this netstack must
    /// be configured to allow IP packets to be routed if it was not destined for this node.
    fn is_routing_enabled(&self, device: Self::DeviceId) -> bool;

    /// Enables or disables IP packet routing on `device`.
    ///
    /// `set_routing_enabled` does nothing if the new routing status, `enabled`, is the same as
    /// the current routing status.
    ///
    /// Note, enabling routing does not mean that `device` will immediately start routing IP
    /// packets. It only means that `device` is allowed to route packets. To route packets, this
    /// netstack must be configured to allow IP packets to be routed if it was not destined for this
    /// node.
    fn set_routing_enabled(&mut self, device: Self::DeviceId, enabled: bool);

    /// Is `device` currently operating as a router?
    ///
    /// Returns `true` if both the `device` has routing enabled AND the netstack
    /// is configured to route packets not destined for it; returns `false`
    /// otherwise.
    fn is_router_device(&self, device: Self::DeviceId) -> bool;
}

impl<I: Ip, C: IpDeviceContext<I>> IpDeviceHandler<I> for C {
    fn initialize_device(&mut self, device: Self::DeviceId) {
        let (protocol, id) = C::get_device_protocol_id(device);
        match protocol {
            DeviceProtocol::Ethernet => {
                <C as IpLinkDeviceHandler<I, EthernetLinkDevice>>::initialize_device(
                    self,
                    id.into(),
                )
            }
        }
    }

    fn deinitialize_device(&mut self, device: Self::DeviceId) {
        let (protocol, id) = C::get_device_protocol_id(device);
        match protocol {
            DeviceProtocol::Ethernet => {
                <C as IpLinkDeviceHandler<I, EthernetLinkDevice>>::deinitialize_device(
                    self,
                    id.into(),
                )
            }
        }
    }

    fn get_ip_addr_subnet(&self, device: Self::DeviceId) -> Option<AddrSubnet<I::Addr>> {
        get_ip_addr_subnet(self, device)
    }

    fn get_ip_addr_subnets(
        &self,
        device: Self::DeviceId,
    ) -> FilterMap<
        Iter<AddressEntry<I::Addr, Self::Instant>>,
        fn(&AddressEntry<I::Addr, Self::Instant>) -> Option<AddrSubnet<I::Addr>>,
    > {
        get_ip_addr_subnets(self, device)
    }

    fn get_ip_addr_subnets_with_tentative(
        &self,
        device: Self::DeviceId,
    ) -> FilterMap<
        Iter<AddressEntry<I::Addr, Self::Instant>>,
        fn(&AddressEntry<I::Addr, Self::Instant>) -> Option<Tentative<AddrSubnet<I::Addr>>>,
    > {
        get_ip_addr_subnets_with_tentative(self, device)
    }

    fn get_ip_addr_state(
        &self,
        device: Self::DeviceId,
        addr: &SpecifiedAddr<I::Addr>,
    ) -> Option<AddressState> {
        get_ip_addr_state(self, device, addr)
    }

    fn add_ip_addr_subnet(
        &mut self,
        device: Self::DeviceId,
        addr_sub: AddrSubnet<I::Addr>,
    ) -> Result<(), AddressError> {
        add_ip_addr_subnet(self, device, addr_sub)
    }

    fn del_ip_addr(
        &mut self,
        device: Self::DeviceId,
        addr: &SpecifiedAddr<I::Addr>,
    ) -> Result<(), AddressError> {
        del_ip_addr(self, device, addr)
    }

    fn get_link_local_addr(&self, device: Self::DeviceId) -> Option<LinkLocalAddr<I::Addr>> {
        get_link_local_addr(self, device)
    }

    fn join_ip_multicast(
        &mut self,
        device: Self::DeviceId,
        multicast_addr: MulticastAddr<I::Addr>,
    ) {
        join_ip_multicast(self, device, multicast_addr)
    }

    fn leave_ip_multicast(
        &mut self,
        device: Self::DeviceId,
        multicast_addr: MulticastAddr<I::Addr>,
    ) {
        leave_ip_multicast(self, device, multicast_addr)
    }

    fn is_in_ip_multicast(
        &self,
        device: Self::DeviceId,
        multicast_addr: MulticastAddr<I::Addr>,
    ) -> bool {
        is_in_ip_multicast(self, device, multicast_addr)
    }

    fn get_mtu(&self, device: Self::DeviceId) -> u32 {
        get_mtu::<_, I>(self, device)
    }

    fn get_hop_limit(&self, device: Self::DeviceId) -> NonZeroU8 {
        get_hop_limit::<_, I>(self, device)
    }

    fn is_routing_enabled(&self, device: Self::DeviceId) -> bool {
        is_routing_enabled::<_, I>(self, device)
    }

    fn set_routing_enabled(&mut self, device: Self::DeviceId, enabled: bool) {
        set_routing_enabled::<_, I>(self, device, enabled)
    }

    fn is_router_device(&self, device: Self::DeviceId) -> bool {
        is_router_device::<_, I>(self, device)
    }
}

/// A dummy device ID for use in testing.
///
/// `DummyDeviceId` is provided for use in implementing
/// `IpDeviceIdContext::DeviceId` in tests. Unlike `()`, it implements the
/// `Display` trait, which is a requirement of `IpDeviceIdContext::DeviceId`.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
#[cfg(test)]
pub(crate) struct DummyDeviceId;

#[cfg(test)]
impl Display for DummyDeviceId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "DummyDeviceId")
    }
}

fn get_ip_addr_subnet<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &C,
    device: <C as IpDeviceIdContext>::DeviceId,
) -> Option<AddrSubnet<A>> {
    get_ip_addr_subnets(ctx, device).nth(0)
}

#[specialize_ip_address]
fn get_ip_addr_subnets<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
) -> FilterMap<
    Iter<AddressEntry<A, C::Instant>>,
    fn(&AddressEntry<A, C::Instant>) -> Option<AddrSubnet<A>>,
> {
    let state = ctx.get_state_with(device_id);

    #[ipv4addr]
    let addresses = &state.ipv4_addr_sub;

    #[ipv6addr]
    let addresses = &state.ipv6_addr_sub;

    addresses.iter().filter_map(
        |a| {
            if a.state().is_assigned() {
                Some(*a.addr_sub())
            } else {
                None
            }
        },
    )
}

#[specialize_ip_address]
fn get_ip_addr_subnets_with_tentative<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
) -> FilterMap<
    Iter<AddressEntry<A, C::Instant>>,
    fn(&AddressEntry<A, C::Instant>) -> Option<Tentative<AddrSubnet<A>>>,
> {
    let state = ctx.get_state_with(device_id);

    #[ipv4addr]
    let addresses = &state.ipv4_addr_sub;

    #[ipv6addr]
    let addresses = &state.ipv6_addr_sub;

    addresses.iter().filter_map(|a| match a.state() {
        AddressState::Assigned => Some(Tentative::new_permanent(*a.addr_sub())),
        AddressState::Tentative => Some(Tentative::new_tentative(*a.addr_sub())),
        AddressState::Deprecated => None,
    })
}

fn get_ip_addr_state<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    addr: &SpecifiedAddr<A>,
) -> Option<AddressState> {
    get_ip_addr_state_inner(ctx, device_id, &addr.get(), None)
}

#[specialize_ip_address]
fn get_ip_addr_state_inner<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    addr: &A,
    configuration_type: Option<AddressConfigurationType>,
) -> Option<AddressState> {
    let state = ctx.get_state_with(device_id);

    #[ipv4addr]
    return state.ipv4_addr_sub.iter().find_map(|a| {
        if a.addr_sub().addr().get() == *addr
            && configuration_type.map_or(true, |x| x == a.configuration_type())
        {
            Some(a.state())
        } else {
            None
        }
    });

    #[ipv6addr]
    return state
        .ipv6_addr_sub
        .iter()
        .find_map(|a| {
            if a.addr_sub().addr().get() == *addr
                && configuration_type.map_or(true, |x| x == a.configuration_type())
            {
                Some(a.state())
            } else {
                None
            }
        })
        .or_else(|| {
            state.ipv6_link_local_addr_sub.as_ref().and_then(|a| {
                if a.addr_sub().addr().get() == *addr
                    && configuration_type.map_or(true, |x| x == a.configuration_type())
                {
                    Some(a.state())
                } else {
                    None
                }
            })
        });
}

#[specialize_ip]
fn initialize_device_inner<C: IpDeviceContext<I>, I: Ip>(
    ctx: &mut C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
) {
    #[ipv6]
    {
        // All nodes should join the all-nodes multicast group.
        join_ip_multicast(
            ctx,
            device_id,
            MulticastAddr::new(Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS).unwrap(),
        );

        let (protocol, id) = C::get_device_protocol_id(device_id);

        //
        // Assign a link-local address.
        //

        let state = ctx.get_state_mut_with(device_id);

        // Must not have a link local address yet.
        assert!(state.ipv6_link_local_addr_sub.is_none());

        let addr = match protocol {
            DeviceProtocol::Ethernet => {
                <C as LinkDeviceHandler<EthernetLinkDevice>>::get_link_layer_addr(ctx, id.into())
                    .to_ipv6_link_local()
                    .get()
            }
        };

        // First, join the solicited-node multicast group for the link-local address.
        join_ip_multicast(ctx, device_id, addr.to_solicited_node_address());

        // Associate the link-local address to the device, and mark it as Tentative, configured by
        // SLAAC, and not set to expire.
        let state = ctx.get_state_mut_with(device_id);
        state.ipv6_link_local_addr_sub = Some(AddressEntry::new(
            AddrSubnet::new(addr, 128).unwrap(),
            AddressState::Tentative,
            AddressConfigurationType::Slaac,
            None,
        ));

        // Perform Duplicate Address Detection on the link-local address.
        match protocol {
            DeviceProtocol::Ethernet => {
                <C as IpLinkDeviceHandler<Ipv6, EthernetLinkDevice>>::added_ip_addr(
                    ctx,
                    id.into(),
                    SpecifiedAddr::new(addr).unwrap(),
                )
            }
        }
    }
}

fn add_ip_addr_subnet<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &mut C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    addr_sub: AddrSubnet<A>,
) -> Result<(), AddressError> {
    add_ip_addr_subnet_inner(ctx, device_id, addr_sub, AddressConfigurationType::Manual, None)
}

#[specialize_ip_address]
fn add_ip_addr_subnet_inner<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &mut C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    addr_sub: AddrSubnet<A>,
    configuration_type: AddressConfigurationType,
    valid_until: Option<C::Instant>,
) -> Result<(), AddressError> {
    let addr = addr_sub.addr().get();

    // MUST NOT be link-local.
    assert!(!addr.is_linklocal());

    if get_ip_addr_state_inner(ctx, device_id, &addr, None).is_some() {
        return Err(AddressError::AlreadyExists);
    }

    let state = ctx.get_state_mut_with(device_id);

    #[ipv4addr]
    state.ipv4_addr_sub.push(AddressEntry::new(
        addr_sub,
        AddressState::Assigned,
        configuration_type,
        valid_until,
    ));

    #[ipv6addr]
    {
        // First, join the solicited-node multicast group.
        join_ip_multicast(ctx, device_id, addr.to_solicited_node_address());

        let state = ctx.get_state_mut_with(device_id);

        state.ipv6_addr_sub.push(AddressEntry::new(
            addr_sub,
            AddressState::Tentative,
            configuration_type,
            valid_until,
        ));

        // Do Duplicate Address Detection on `addr`.
        let (protocol, id) = C::get_device_protocol_id(device_id);
        match protocol {
            DeviceProtocol::Ethernet => {
                <C as IpLinkDeviceHandler<Ipv6, EthernetLinkDevice>>::added_ip_addr(
                    ctx,
                    id.into(),
                    addr_sub.addr(),
                )
            }
        }
    }

    Ok(())
}

fn del_ip_addr<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &mut C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    addr: &SpecifiedAddr<A>,
) -> Result<(), AddressError> {
    del_ip_addr_inner(ctx, device_id, &addr.get(), None)
}

#[specialize_ip_address]
fn del_ip_addr_inner<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &mut C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    addr: &A,
    configuration_type: Option<AddressConfigurationType>,
) -> Result<(), AddressError> {
    // MUST NOT be link-local.
    assert!(!addr.is_linklocal());

    #[ipv4addr]
    {
        let state = ctx.get_state_mut_with(device_id);

        let original_size = state.ipv4_addr_sub.len();
        if let Some(configuration_type) = configuration_type {
            state.ipv4_addr_sub.retain(|x| {
                (x.addr_sub().addr().get() != *addr)
                    && (x.configuration_type() == configuration_type)
            });
        } else {
            state.ipv4_addr_sub.retain(|x| x.addr_sub().addr().get() != *addr);
        }

        let new_size = state.ipv4_addr_sub.len();

        if new_size == original_size {
            return Err(AddressError::NotFound);
        }

        assert_eq!(original_size - new_size, 1);

        Ok(())
    }

    #[ipv6addr]
    {
        if let Some(state) = get_ip_addr_state_inner(ctx, device_id, addr, configuration_type) {
            if state.is_tentative() {
                // Cancel current duplicate address detection for `addr` as we are
                // removing this IP.
                //
                // `cancel_duplicate_address_detection` may panic if we are not
                // performing DAD on `addr`. However, we will only reach here
                // if `addr` is marked as tentative. If `addr` is marked as
                // tentative, then we know that we are performing DAD on it.
                // Given this, we know `cancel_duplicate_address_detection` will
                // not panic.
                let (protocol, id) = C::get_device_protocol_id(device_id);
                match protocol {
                    DeviceProtocol::Ethernet => {
                        <C as IpLinkDeviceHandler<Ipv6, EthernetLinkDevice>>::removed_ip_addr(
                            ctx,
                            id.into(),
                            SpecifiedAddr::new(*addr).unwrap(),
                        )
                    }
                }
            }
        } else {
            return Err(AddressError::NotFound);
        }

        let state = ctx.get_state_mut_with(device_id);

        let original_size = state.ipv6_addr_sub.len();
        state.ipv6_addr_sub.retain(|x| x.addr_sub().addr().get() != *addr);
        let new_size = state.ipv6_addr_sub.len();

        // Since we just checked earlier if we had the address, we must have removed it
        // now.
        assert_eq!(original_size - new_size, 1);

        // Leave the the solicited-node multicast group.
        leave_ip_multicast(ctx, device_id, addr.to_solicited_node_address());

        Ok(())
    }
}

#[specialize_ip_address]
fn get_link_local_addr<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
) -> Option<LinkLocalAddr<A>> {
    // We do not currently support IPv4 link-local addresses.
    #[ipv4addr]
    return None;

    // TODO(brunodalbo) the link local address is subject to the same collision
    //  verifications as prefix global addresses, we should keep a state machine
    //  about that check and cache the adopted address. For now, we just compose
    //  the link-local from the ethernet MAC.
    #[ipv6addr]
    return ctx
        .get_state_with(device_id)
        .ipv6_link_local_addr_sub
        .as_ref()
        .map(|a| if a.state().is_assigned() { Some(a.addr_sub().addr()) } else { None })
        .unwrap_or(None);
}

#[specialize_ip_address]
fn join_ip_multicast<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &mut C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    multicast_addr: MulticastAddr<A>,
) {
    let device_state = ctx.get_state_mut_with(device_id);

    #[ipv4addr]
    let groups = &mut device_state.ipv4_multicast_groups;

    #[ipv6addr]
    let groups = &mut device_state.ipv6_multicast_groups;

    let counter = groups.entry(multicast_addr).or_insert(0);
    *counter += 1;

    if *counter == 1 {
        let mac = MulticastAddr::from(&multicast_addr);

        trace!(
            "join_ip_multicast: joining IP multicast {:?} and MAC multicast {:?}",
            multicast_addr,
            mac
        );

        // TODO(ghanan): Make `EventDispatcher` aware of this to maintain a single source of truth.
        let (protocol, id) = C::get_device_protocol_id(device_id);
        match protocol {
            DeviceProtocol::Ethernet => {
                <C as LinkDeviceHandler<EthernetLinkDevice>>::join_link_multicast(
                    ctx,
                    id.into(),
                    mac,
                )
            }
        }
    } else {
        trace!(
            "ethernet::join_ip_multicast: already joinined IP multicast {:?}, counter = {}",
            multicast_addr,
            *counter,
        );
    }
}

#[specialize_ip_address]
fn leave_ip_multicast<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &mut C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    multicast_addr: MulticastAddr<A>,
) {
    let device_state = ctx.get_state_mut_with(device_id);
    let mac = MulticastAddr::from(&multicast_addr);

    #[ipv4addr]
    let groups = &mut device_state.ipv4_multicast_groups;

    #[ipv6addr]
    let groups = &mut device_state.ipv6_multicast_groups;

    // Will panic if `device_id` has not yet joined the multicast address.
    let counter =
        groups.get_mut(&multicast_addr).expect("cannot leave not-yet-joined multicast group");

    if *counter == 1 {
        let mac = MulticastAddr::from(&multicast_addr);

        trace!(
            "ethernet::leave_ip_multicast: leaving IP multicast {:?} and MAC multicast {:?}",
            multicast_addr,
            mac
        );

        groups.remove(&multicast_addr);

        // TODO(ghanan): Make `EventDispatcher` aware of this to maintain a single source of truth.
        let (protocol, id) = C::get_device_protocol_id(device_id);
        match protocol {
            DeviceProtocol::Ethernet => {
                <C as LinkDeviceHandler<EthernetLinkDevice>>::leave_link_multicast(
                    ctx,
                    id.into(),
                    mac,
                )
            }
        }
    } else {
        *counter -= 1;

        trace!(
            "ethernet::leave_ip_multicast: not leaving IP multicast {:?} as there are still listeners for it, counter = {}",
            multicast_addr,
            *counter,
        );
    }
}

#[specialize_ip_address]
fn is_in_ip_multicast<C: IpDeviceContext<A::Version>, A: IpAddress>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
    multicast_addr: MulticastAddr<A>,
) -> bool {
    #[ipv4addr]
    return ctx.get_state_with(device_id).ipv4_multicast_groups.contains_key(&multicast_addr);

    #[ipv6addr]
    return ctx.get_state_with(device_id).ipv6_multicast_groups.contains_key(&multicast_addr);
}

fn get_mtu<C: IpDeviceContext<I>, I: Ip>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
) -> u32 {
    let (protocol, id) = C::get_device_protocol_id(device_id);
    match protocol {
        DeviceProtocol::Ethernet => {
            <C as LinkDeviceHandler<EthernetLinkDevice>>::get_mtu(ctx, id.into())
        }
    }
}

#[specialize_ip]
fn get_hop_limit<C: IpDeviceContext<I>, I: Ip>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
) -> NonZeroU8 {
    #[ipv4]
    return NonZeroU8::new(crate::ip::DEFAULT_TTL).unwrap();

    #[ipv6]
    return ctx.get_state_with(device_id).ipv6_hop_limit;
}

#[specialize_ip]
fn is_routing_enabled<C: IpDeviceContext<I>, I: Ip>(
    ctx: &C,
    device_id: <C as IpDeviceIdContext>::DeviceId,
) -> bool {
    #[ipv4]
    return ctx.get_state_with(device_id).route_ipv4;

    #[ipv6]
    return ctx.get_state_with(device_id).route_ipv6;
}

#[specialize_ip]
fn set_routing_enabled<C: IpDeviceContext<I>, I: Ip>(
    ctx: &mut C,
    device: <C as IpDeviceIdContext>::DeviceId,
    enabled: bool,
) {
    let (protocol, id) = C::get_device_protocol_id(device);
    match protocol {
        DeviceProtocol::Ethernet => {
            <C as IpLinkDeviceHandler<I, EthernetLinkDevice>>::set_routing_enabled(
                ctx,
                id.into(),
                enabled,
            )
        }
    }
}

#[specialize_ip]
fn set_routing_enabled_inner<C: IpDeviceContext<I>, I: Ip>(
    ctx: &mut C,
    device: <C as IpDeviceIdContext>::DeviceId,
    enabled: bool,
) {
    // TODO(ghanan): We cannot directly do `I::VERSION` in the `trace!` calls because of a bug in
    //               specialize_ip_macro where it does not properly replace `I` with `Self`. Once
    //               this is fixed, change this.
    let version = I::VERSION;

    if is_routing_enabled::<_, I>(ctx, device) == enabled {
        trace!(
            "set_routing_enabled: {:?} routing status unchanged for device {:?}",
            version,
            device
        );
        return;
    }

    #[ipv4]
    ctx.get_state_mut_with(device).route_ipv4 = enabled;

    #[ipv6]
    {
        let ip_routing = <C as IpHandler>::is_routing_enabled::<Ipv6>(ctx);
        let (protocol, id) = C::get_device_protocol_id(device);

        if enabled {
            // Actually update the routing flag.
            ctx.get_state_mut_with(device).route_ipv6 = true;

            if ip_routing {
                // Now that `device` is a router, join the all-routers multicast group.
                join_ip_multicast(
                    ctx,
                    device,
                    MulticastAddr::new(Ipv6::ALL_ROUTERS_LINK_LOCAL_ADDRESS).unwrap(),
                );
            }
        } else {
            if ip_routing {
                // Now that `device` is a host, leave the all-routers multicast group.
                leave_ip_multicast(
                    ctx,
                    device,
                    MulticastAddr::new(Ipv6::ALL_ROUTERS_LINK_LOCAL_ADDRESS).unwrap(),
                );
            }

            // Actually update the routing flag.
            ctx.get_state_mut_with(device).route_ipv6 = false;
        }
    }
}

fn is_router_device<C: IpDeviceContext<I>, I: Ip>(
    ctx: &C,
    device: <C as IpDeviceIdContext>::DeviceId,
) -> bool {
    <C as IpHandler>::is_routing_enabled::<I>(ctx) && is_routing_enabled::<_, I>(ctx, device)
}
