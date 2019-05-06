// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Minimal type-safe definitions of the virtio data structures.
//!
//! Contains definitions and type-safe accessors and manipulators of the virtio data structures.
//! For the leaf data structures like [descriptors](ring::Desc) these definitions are simply the in
//! memory layout as a Rust `struct`.
//!
//! Unfortunately the virtqueues are a variable sized data structure, whose length is not known till
//! run time as the size is determined by the driver. Representing the virtqueue as 'just' a Rust
//! `struct` is therefore not possible.
//!
//! Minimally a virtqueue could be represented as three pointers and a size. Although such a
//! definition is not very ergonomic and would make the implementation much more complex. Instead,
//! multiple slices and pointers are used across two different structs. Two structs are used as it
//! allows for separating the [device](ring::Device) owned and [driver](ring::Driver) owned portions
//! of the virtqueue into separate portions with their correct mutability.
//!
//! Overall this results in effectively duplicating the size of the virtqueue a few times in the
//! slice fat pointers and a couple of additional pointers. This is deemed a small memory overhead
//! for the benefit of readability and ease of use.
//!
//! Due to the split into the [`Driver`](ring::Driver) and [`Device`](ring::Device) structs there is
//! no specifically named `virtqueue` in this module. The [Queue](queue::Queue) builds on the
//! [`Driver`](ring::Driver) and [`Device`](ring::Device) to build useful virtqueue functionality.
//!
//! These abstractions are intended to be type-safe, but not enforce correct implementation of the
//! virtio protocols. As such reading the [virtio specification](https://docs.oasis-open.org/virtio/virtio/v1.1/csprd01/virtio-v1.1-csprd01.html)
//! is required to correctly use this module. Most likely you do not want to use these directly and
//! want to use the higher level [`queue`], and [`desc`] modules that provide less mis-usable
//! wrappers.
//!
//! TODO: Expose ability for devices to control notification suppression.

use std::{
    mem, slice,
    sync::atomic::{self, AtomicU16},
};

/// Descriptor has a next field.
const VRING_DESC_F_NEXT: u16 = 0b1;
/// Descriptor is device write-only (otherwise device read-only).
const VRING_DESC_F_WRITE: u16 = 0b10;
/// Descriptor contains a list of buffer descriptors.
const VRING_DESC_F_INDIRECT: u16 = 0b100;

/// Virtio descriptor data structure
///
/// Represents the in memory format of virtio descriptors and provides some accessors.
#[repr(C)]
#[derive(Debug, Clone)]
pub struct Desc {
    addr: u64,
    len: u32,
    // This is not bitflags! as it may contain additional bits that we do not define
    // and so would violate the bitflags type safety.
    flags: u16,
    next: u16,
}

impl Desc {
    /// Returns whether the [next](VRING_DESC_F_NEXT) bit is set.
    ///
    /// Typically the [next](#next) method is 
    pub fn has_next(&self) -> bool {
        self.flags & VRING_DESC_F_NEXT != 0
    }
    /// Returns whether the [indirect](VRING_DESC_F_INDIRECT) bit is set.
    pub fn is_indirect(&self) -> bool {
        self.flags & VRING_DESC_F_INDIRECT != 0
    }
    /// Returns whether the [write](VRING_DESC_F_WRITE) bit is set.
    ///
    /// This flag should be ignored when [is_indirect](#is_indirect) is true.
    pub fn write_only(&self) -> bool {
        self.flags & VRING_DESC_F_WRITE != 0
    }
    /// Returns the next descriptor if there is one, otherwise a `None`.
    pub fn next(&self) -> Option<u16> {
        if self.has_next() {
            Some(self.next)
        } else {
            None
        }
    }
    /// Returns the guest (address, length) pair representing the contents of this descriptor.
    pub fn data(&self) -> (u64, u32) {
        (self.addr, self.len)
    }
}

#[repr(C)]
#[derive(Debug)]
struct Header {
    flags: u16,
    idx: AtomicU16,
}

impl Header {
    fn are_notifications_suppressed(&self) -> bool {
        self.flags == 1
    }
    fn load_idx(&self) -> u16 {
        self.idx.load(atomic::Ordering::Acquire)
    }
    fn store_idx(&mut self, idx: u16) {
        self.idx.store(idx, atomic::Ordering::Release)
    }
    /// Changes flags to suppress notifications.
    ///
    /// Not permitted if VIRTIO_F_EVENT_IDX feature was negotiated.
    #[allow(dead_code)]
    fn suppress_notifications(&mut self) {
        self.flags = 1;
    }
    /// Change flags to enable notifications.
    fn enable_notifications(&mut self) {
        self.flags = 0;
    }
}

/// Representation of driver owned data.
///
/// Contents of this `struct` are expected to be being modified in parallel by a driver in a guest.
/// Provides methods for safely querying, using appropriate memory barriers, items published by the
/// driver.
pub struct Driver<'a> {
    desc: &'a [Desc],
    avail_header: &'a Header,
    avail: &'a [u16],
    used_event_index: &'a u16,
}

impl<'a> Driver<'a> {
    /// How many bytes the the avail ring should be for the given `count`.
    ///
    /// Provides an easy way to calculate the correct size of the slice for passing to [`new`]
    pub const fn avail_len_for_count(count: u16) -> usize {
        mem::size_of::<Header>() + mem::size_of::<u16>() * (count as usize + 1)
    }
    /// Construct a [`Driver`] using the provided memory for descriptor and available rings.
    ///
    /// Provided slices must be correctly sized to represent the same power of two queue size,
    /// otherwise a `None` is returned.
    pub fn new(desc: &'a [u8], avail: &'a [u8]) -> Option<Self> {
        let count = desc.len() / mem::size_of::<Desc>();
        if !count.is_power_of_two() {
            return None;
        }

        let desc = unsafe { slice::from_raw_parts(desc.as_ptr() as *const Desc, count) };

        if avail.len() < mem::size_of::<Header>() {
            return None;
        }
        let (avail_header, rest) = avail.split_at(mem::size_of::<Header>());
        let avail_header = unsafe { &*(avail_header.as_ptr() as *const Header) };

        // Reinterpret the rest as a [u16], with the last one being the used_event_index
        if rest.len() != mem::size_of::<u16>() * (count + 1) {
            return None;
        }
        let rest = unsafe { slice::from_raw_parts(rest.as_ptr() as *const u16, count + 1) };
        // We know that `rest` is not empty, having just constructed it with at least one
        // element, so we can safely unwrap.
        let (used_event_index, avail) = rest.split_last().unwrap();
        Some(Self { desc, avail_header, avail, used_event_index })
    }
    /// Query if a descriptor chain has been published with the given index.
    ///
    /// If a chain has been published by the driver then returns the index of the first descriptor
    /// in the chain. Otherwise returns a `None`.
    pub fn get_avail(&self, next_index: u16) -> Option<u16> {
        if next_index != self.avail_header.load_idx() {
            // unwrap is used here as the get cannot fail as we explicitly wrap the index using
            // the length.
            let index = self.avail.get(next_index as usize % self.avail.len()).unwrap();
            Some(*index)
        } else {
            None
        }
    }
    /// Request a reference to a descriptor by index.
    pub fn get_desc(&self, index: u16) -> Option<&'a Desc> {
        self.desc.get(index as usize)
    }
    /// Determines if the driver has requested a notification for the given descriptor submission.
    ///
    /// Queries the information published by the driver to determine whether or not it would like a
    /// notification for the given `submitted` descriptor by the [`Device`]. As the [`Driver`] holds
    /// no host state whether the `VIRTIO_F_EVENT_IDX` feature was negotiated must be passed in.
    pub fn needs_notification(&self, feature_event_idx: bool, submitted: u16) -> bool {
        if feature_event_idx {
            submitted == *self.used_event_index
        } else {
            !self.avail_header.are_notifications_suppressed()
        }
    }
    /// Returns the size of the descriptor and available rings.
    ///
    /// These rings are, by definition, the same size.
    pub fn count(&self) -> u16 {
        self.avail.len() as u16
    }
}

/// Representation of an entry in the used ring.
///
/// The only purpose [`Used`] has is to be passed to [insert_used](Device::insert_used) to be
/// copied into the used ring. As a result the only provided method is [new](Used::new) and there
/// are no accessors, as the driver is the one who will be accessing it.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct Used {
    /// Index of start of used descriptor chain.
    ///
    /// For padding reasons `id` in this structure is 32-bits, although it will never exceed
    /// an actual 16-bit descriptor index.
    id: u32,
    /// Total length of the descriptor chain which was used (written to).
    len: u32,
}

impl Used {
    /// Constructs a new used entry.
    ///
    /// `id` is the index of the first descriptor in the chain being returned and `len` is the
    /// total number of bytes written to any writable descriptors in the chain.
    pub fn new(id: u16, len: u32) -> Used {
        Used { id: id.into(), len }
    }
}

/// Represents the device owned data.
///
/// Contents of this struct are expected to be modified by the device and so are mutable. Provided
/// methods allow for safely publishing data to the driver using appropriate memory barriers.
pub struct Device<'a> {
    used_header: &'a mut Header,
    used: &'a mut [Used],
    #[allow(dead_code)]
    avail_event_index: &'a mut u16,
}

impl<'a> Device<'a> {
    /// How many bytes the the avail ring should be for the given `count`.
    ///
    /// Provides an easy way to calculate the correct size of the slice for passing to [`new`]
    pub const fn used_len_for_count(count: u16) -> usize {
        mem::size_of::<Header>()
            + mem::size_of::<Used>() * count as usize
            + mem::size_of::<u16>()
    }
    /// Construct a [`Device`] using the provided memory for the used ring.
    ///
    /// Provided slices must be correctly sized to represent a power of two queue size, otherwise a
    /// `None` is returned.
    pub fn new(used: &'a mut [u8]) -> Option<Self> {
        if used.len() < mem::size_of::<Header>() {
            return None;
        }
        let (used_header, rest) = used.split_at_mut(mem::size_of::<Header>());
        let used_header = unsafe { &mut *(used_header.as_mut_ptr() as *mut Header) };

        // Take the last u16 from what is remaining as avail_event_index
        if rest.len() < mem::size_of::<u16>() {
            return None;
        }
        let (used, avail_event_index) = rest.split_at_mut(rest.len() - mem::size_of::<u16>());
        let avail_event_index = unsafe { &mut *(avail_event_index.as_mut_ptr() as *mut u16) };

        // Reinterpret used as [Used]
        let used = unsafe {
            slice::from_raw_parts_mut(
                used.as_mut_ptr() as *mut Used,
                used.len() / mem::size_of::<Used>(),
            )
        };
        if !used.len().is_power_of_two() {
            return None;
        }

        // Start with notifications from the driver enabled by default.
        used_header.enable_notifications();

        Some(Self { used_header, used, avail_event_index })
    }
    /// Returns the size of the used ring.
    pub fn count(&self) -> u16 {
        self.used.len() as u16
    }
    /// Add a descriptor chain to the used ring.
    ///
    /// After calling this the descriptor is not yet visible to the driver. To make it visible
    /// [`publish_used`](#publish_used) must be called. Chains are not implicitly published to allow
    /// for batching the return of chains.
    ///
    /// For consistency between this and [`publish_used`](#publish_used) `index` will automatically
    /// be wrapped to the queue length.
    pub fn insert_used(&mut self, used: Used, index: u16) {
        // unwrap is safe to use as the index is being wrapped to the length and new would have
        // forbidden creation of a zero sized ring.
        let used_slot = self
            .used
            .get_mut(index as usize % self.used.len()).unwrap();
        *used_slot = used;
    }
    /// Publish the avail ring up to the provided `index` to the driver.
    pub fn publish_used(&mut self, index: u16) {
        self.used_header.store_idx(index);
    }
}

#[cfg(test)]
mod tests {
    use {
        std::mem::size_of,
        super::*,
    };

    #[test]
    fn test_good_size() {
        // Declare memory for queues with a queue size of 8.
        // These should be on different pages, but that isn't enforced in our implementation so
        // we get away with just stack declarations.
        let desc: [u8; size_of::<Desc>() * 8] = [0; size_of::<Desc>() * 8];
        let avail: [u8; Driver::avail_len_for_count(8)] = [0; Driver::avail_len_for_count(8)];
        let mut used: [u8; Device::used_len_for_count(8)] = [0; Device::used_len_for_count(8)];
        assert!(Driver::new(&desc[..], &avail[..]).is_some());
        assert!(Device::new(&mut used[..]).is_some());
    }
    #[test]
    fn test_bad_size() {
        // Declare memory for queue size of 3, which is not a power of two.
        let desc: [u8; size_of::<Desc>() * 3] = [0; size_of::<Desc>() * 3];
        let avail: [u8; Driver::avail_len_for_count(3)] = [0; Driver::avail_len_for_count(3)];
        let mut used: [u8; Device::used_len_for_count(3)] = [0; Device::used_len_for_count(3)];

        assert!(Driver::new(&desc[..], &avail[..]).is_none());
        assert!(Device::new(&mut used[..]).is_none());

        // Try giving a zero length slice as well
        assert!(Driver::new(&desc[0..0], &avail[0..0]).is_none());
        assert!(Device::new(&mut used[0..0]).is_none());
    }
    #[test]
    fn test_driver_notifications() {

    }
}
