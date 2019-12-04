// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Link device (L2) definitions.
//!
//! This module contains definitions of link-layer devices, otherwise known as
//! L2 devices.

use std::fmt::Debug;

use net_types::ethernet::Mac;
use net_types::ip::IpVersion;
use net_types::MulticastAddr;
use packet::BufferMut;
use zerocopy::{AsBytes, FromBytes, Unaligned};

use crate::context::{CounterContext, FrameContext, InstantContext, RngContext, StateContext};
use crate::device::IpLinkDeviceState;

/// The type of address used by a link device.
pub(crate) trait LinkAddress:
    'static + FromBytes + AsBytes + Unaligned + Copy + Clone + Debug + Eq
{
    /// The length of the address in bytes.
    const BYTES_LENGTH: usize;

    /// Returns the underlying bytes of a `LinkAddress`.
    fn bytes(&self) -> &[u8];

    /// Constructs a `LinkLayerAddress` from the provided bytes.
    ///
    /// # Panics
    ///
    /// `from_bytes` may panic if `bytes` is not **exactly** [`BYTES_LENGTH`]
    /// long.
    fn from_bytes(bytes: &[u8]) -> Self;
}

/// A [`LinkAddress`] with a broadcast value.
///
/// A `BroadcastLinkAddress` is a `LinkAddress` for which at least one address
/// is a "broadcast" address, indicating that a frame should be received by all
/// hosts on a link.
pub(crate) trait BroadcastLinkAddress: LinkAddress {
    /// The broadcast address.
    ///
    /// If the addressing scheme supports multiple broadcast addresses, then
    /// there is no requirement as to which one is chosen for this constant.
    const BROADCAST: Self;
}

impl LinkAddress for Mac {
    const BYTES_LENGTH: usize = 6;

    fn bytes(&self) -> &[u8] {
        self.as_ref()
    }

    fn from_bytes(bytes: &[u8]) -> Self {
        // assert that contract is being held:
        debug_assert_eq!(bytes.len(), Self::BYTES_LENGTH);
        let mut b = [0; Self::BYTES_LENGTH];
        b.copy_from_slice(bytes);
        Self::new(b)
    }
}

impl BroadcastLinkAddress for Mac {
    const BROADCAST: Mac = Mac::BROADCAST;
}

/// A link device.
///
/// `LinkDevice` is used to identify a particular link device implementation. It
/// is only intended to exist at the type level, never instantiated at runtime.
pub(crate) trait LinkDevice: 'static + Copy + Clone {
    /// The type of address used to address link devices of this type.
    type Address: LinkAddress;

    /// The type of frames that this device supports.
    type FrameType: From<IpVersion> + Copy;
}

/// An execution context which provides a `DeviceId` type for various device
/// layer internals to share.
pub(crate) trait LinkDeviceIdContext<D: LinkDevice> {
    type DeviceId: Copy + Debug + Eq + From<usize>;
}

pub(crate) trait LinkDeviceContextImpl<D: LinkDevice> {
    type State;
}

pub(super) trait LinkDeviceContext<D: LinkDevice>:
    InstantContext
    + LinkDeviceIdContext<D>
    + LinkDeviceContextImpl<D>
    + CounterContext
    + RngContext
    + StateContext<
        IpLinkDeviceState<
            <Self as InstantContext>::Instant,
            <Self as LinkDeviceContextImpl<D>>::State,
        >,
        <Self as LinkDeviceIdContext<D>>::DeviceId,
    >
{
}

pub(super) trait LinkDeviceHandler<D: LinkDevice>: LinkDeviceIdContext<D> {
    /// Is `device` usable?
    ///
    /// That is, is it either initializing or initialized?
    fn is_device_usable(&self, device: <Self as LinkDeviceIdContext<D>>::DeviceId) -> bool;

    /// Get the link layer address for a device.
    fn get_link_layer_addr(
        &self,
        device_id: <Self as LinkDeviceIdContext<D>>::DeviceId,
    ) -> D::Address;

    /// Get the interface identifier for a device as defined by RFC 4291 section 2.5.1.
    fn get_interface_identifier(
        &self,
        device_id: <Self as LinkDeviceIdContext<D>>::DeviceId,
    ) -> [u8; 8];

    /// Set Link MTU.
    ///
    /// `set_mtu` MAY set the device's new MTU to a value less than `mtu` if the device does not
    /// support using `mtu` as its new MTU. `set_mtu` MUST NOT use a new MTU value that is greater
    /// than `mtu`.
    fn set_mtu(&mut self, device_id: <Self as LinkDeviceIdContext<D>>::DeviceId, mtu: u32);

    fn get_mtu(&self, device_id: <Self as LinkDeviceIdContext<D>>::DeviceId) -> u32;

    fn join_link_multicast(
        &mut self,
        device_id: <Self as LinkDeviceIdContext<D>>::DeviceId,
        multicast_addr: MulticastAddr<D::Address>,
    );
    fn leave_link_multicast(
        &mut self,
        device_id: <Self as LinkDeviceIdContext<D>>::DeviceId,
        multicast_addr: MulticastAddr<D::Address>,
    );
}

// The FrameContext here is so that the link device can call `send_frame` to send out
// the final frame with all link-specific headers.
pub(super) trait BufferLinkDeviceContext<D: LinkDevice, B: BufferMut>:
    LinkDeviceContext<D> + FrameContext<B, <Self as LinkDeviceIdContext<D>>::DeviceId>
{
}

impl<
        D: LinkDevice,
        B: BufferMut,
        C: LinkDeviceContext<D> + FrameContext<B, <Self as LinkDeviceIdContext<D>>::DeviceId>,
    > BufferLinkDeviceContext<D, B> for C
{
}

pub(super) struct LinkFrameMeta<D, A, E> {
    pub(super) device: D,
    pub(super) dst_addr: A,
    pub(super) link_specific: E,
}

// The frame context here is so other contexts can request a frame be sent to some
// remote with some link-layer address.
pub(super) trait BufferLinkDeviceHandler<D: LinkDevice, B: BufferMut>:
    LinkDeviceHandler<D>
    + FrameContext<
        B,
        LinkFrameMeta<<Self as LinkDeviceIdContext<D>>::DeviceId, D::Address, D::FrameType>,
    >
{
}

impl<
        D: LinkDevice,
        B: BufferMut,
        C: LinkDeviceHandler<D>
            + FrameContext<
                B,
                LinkFrameMeta<<Self as LinkDeviceIdContext<D>>::DeviceId, D::Address, D::FrameType>,
            >,
    > BufferLinkDeviceHandler<D, B> for C
{
}
