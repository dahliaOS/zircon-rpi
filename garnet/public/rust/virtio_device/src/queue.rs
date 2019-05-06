// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Virtqueue management wrappers.
//!
//! The [`ring`] module provided the low level memory representation definitions for the virtio
//! data structures. This module takes those and provides the full virtqueue abstraction in the
//! [`Queue`](queue::Queue) object.
//!
//! Two pieces of system specific functionality need to be provided to the [`Queue`](queue::Queue)
//! around how to translate memory addresses from the driver (aka guest) and how to deliver
//! notifications to the driver. For this the [`DriverMem`](queue::DriverMeme) and
//! [`DriverNotify`](queue::DriverNotify) traits are defined, and the [`Queue`](queue::Queue) is
//! generic over them.

use {
    crate::ring,
    crossbeam,
    parking_lot::Mutex,
    std::{
        ops::{DerefMut},
        sync::atomic,
    },
};

#[repr(transparent)]
pub struct DriverAddr(pub usize);

pub trait DriverMem {
    // May fail if an address is invalid. Addresses *shouldn't* overlap but they *may*. It is
    // important to remember that this is inherently shared memory that could be being
    // concurrently modified by another thread.
    // As such it is user responsibility to read to local, perform fences and perform error
    // checking.
    fn translate(&self, addr: DriverAddr, len: u32) -> Option<&'static mut [u8]>;
}

pub trait DriverNotify {
    fn notify(&self);
}

/// Mutable state of a virtqueue.
///
/// This includes both the reference to the [`Device`](ring::Device), which is the memory shared
/// with the guest that we actually need to manipulate, as well as additional state needed for us to
/// correctly implement the virtio protocol.
// Mutable state for the rings.
struct State {
    device_state: ring::Device<'static>,
    // Next index in avail that we expect to be come available
    next: u16,
    // Next index in used that we will publish at
    next_used: u16,
}

impl State {
    fn return_chain(&mut self, desc: u16, written: u32) -> u16 {
        let submit = self.next_used;
        self.device_state.insert_used(ring::Used::new(desc, written), submit);
        self.next_used = submit.wrapping_add(1);
        self.device_state.publish_used(self.next_used);
        // Return the index that we just published.
        submit
    }
}

struct Inner<N> {
    driver_state: ring::Driver<'static>,
    state: Mutex<State>,
    notify: N,
    feature_event_idx: bool,
    sender: crossbeam::Sender<(u16, u32)>,
    receiver: crossbeam::Receiver<(u16, u32)>,
}

impl<N: DriverNotify> Inner<N> {
    fn take_avail(&self) -> Option<u16> {
        let mut state = self.state.lock();
        let ret = self.driver_state.get_avail(state.next);
        if ret.is_some() {
            // TODO: worry about events
            state.next = state.next.wrapping_add(1);
        }
        drop(state);
        self.drain_channel();
        ret
    }

    fn return_chain_internal<T: DerefMut<Target = State>>(
        &self,
        state: &mut T,
        desc: u16,
        written: u32,
    ) {
        let submitted = state.return_chain(desc, written);
        // Must ensure the read of flags or used_event occurs *after* we have returned the chain
        // and published the index. We also need to ensure that in the event we do send an
        // interrupt that any state and idx updates have been written, so therefore this becomes
        // acquire and release.
        atomic::fence(atomic::Ordering::AcqRel);
        if self.driver_state.needs_notification(self.feature_event_idx, submitted) {
            self.notify.notify();
        }
    }

    fn return_chain(&self, desc: u16, written: u32) {
        if let Some(mut guard) = self.state.try_lock() {
            self.return_chain_internal(&mut guard, desc, written);
        } else {
            self.sender
                .try_send((desc, written))
                .expect("Sending on unbounded channel should not fail");
        }
        // Resolve any races and drain the channel.
        self.drain_channel();
    }

    fn drain_channel(&self) {
        while !self.receiver.is_empty() {
            if let Some(mut guard) = self.state.try_lock() {
                while let Ok((desc, written)) = self.receiver.try_recv() {
                    self.return_chain_internal(&mut guard, desc, written);
                }
            } else {
                return;
            }
        }
    }
}

pub struct Queue<N>(std::sync::Arc<Inner<N>>);

#[derive(Fail, Debug)]
#[fail(display = "Slice parameter had an incorrect slice")]
pub struct IncorrectSliceSize;

impl<N> Queue<N> {
    pub fn new(
        desc: &'static [u8],
        avail: &'static [u8],
        used: &'static mut [u8],
        notify: N,
    ) -> Result<Queue<N>, IncorrectSliceSize> {
        let driver_state = ring::Driver::new(desc, avail).ok_or(IncorrectSliceSize)?;
        let device_state = ring::Device::new(used).ok_or(IncorrectSliceSize)?;
        if driver_state.count() != device_state.count() {
            return Err(IncorrectSliceSize);
        }
        let queue_state = State { device_state, next: 0, next_used: 0 };
        let (sender, receiver) = crossbeam::unbounded();
        let queue = Inner {
            driver_state,
            state: Mutex::new(queue_state),
            notify,
            feature_event_idx: false,
            sender,
            receiver,
        };
        Ok(Queue(std::sync::Arc::new(queue)))
    }
}
impl<N> Clone for Queue<N> {
    fn clone(&self) -> Self {
        Queue(self.0.clone())
    }
}

impl<N: DriverNotify> Queue<N> {
    pub fn next_desc(&self) -> Option<DescChain<N>> {
        if let Some(desc_index) = self.0.take_avail() {
            Some(DescChain { queue: self.clone(), first_desc: desc_index, written: 0 })
        } else {
            None
        }
    }
}

// This does not implement Deref since technically writable descriptors should
// *not* be read from, and so a blanket Deref actually makes no sense.
pub enum Desc<'a> {
    Readable(&'a [u8]),
    Writable(&'a mut [u8]),
}

#[derive(Fail, Debug)]
#[fail(display = "Memory range at {} of len {} not found in host", addr, len)]
pub struct MemoryNotFound{addr: usize, len: u32}

impl<'a> Desc<'a> {
    pub fn new<M: DriverMem>(desc: &'static ring::Desc, mem: &M) -> Result<Desc<'a>, MemoryNotFound> {
        let (addr, len) = desc.data();
        let addr = addr as usize;
        let slice = mem.translate(DriverAddr(addr), len).ok_or(MemoryNotFound{addr, len})?;
        Ok(if desc.write_only() {
            Desc::Writable(slice)
        } else {
            Desc::Readable(slice)
        })
    }
    pub fn len(&self) -> usize {
        match self {
            Desc::Readable(mem) => mem.len(),
            Desc::Writable(mem) => mem.len(),
        }
    }
}

pub struct DescChainIter<'a, N: DriverNotify, M> {
    queue: Queue<N>,
    desc: Option<u16>,
    mem: &'a M,
    chain: &'a mut DescChain<N>,
}

impl<'a, N: DriverNotify, M: DriverMem> Iterator for DescChainIter<'a, N, M> {
    type Item = Result<Desc<'a>, MemoryNotFound>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(ret) = self.desc {
            let desc = self.queue.0.driver_state.get_desc(ret);
            self.desc = desc.clone().and_then(|d| d.next());
            desc.map(|d| Desc::new(d, self.mem))
        } else {
            None
        }
    }
}

impl<'a, N: DriverNotify, M: DriverMem> DescChainIter<'a, N, M> {
    pub fn get_chain(&mut self) -> &mut DescChain<N> {
        self.chain
    }
    /// # Safety
    ///
    /// `get_chain` must only be used from one of the cloner or clonee, otherwise there will
    /// be a memory race.
    /// TODO: err won't this allow for two iterators to return writable descriptors?
    pub unsafe fn clone(&self) -> DescChainIter<'a, N, M> {
        DescChainIter {queue: self.queue.clone(), desc: self.desc, mem: self.mem, chain:
            &mut*(self.chain as *const DescChain<N> as *mut DescChain<N>)
        }
    }
}

pub struct DescChain<N: DriverNotify> {
    queue: Queue<N>,
    first_desc: u16,
    written: u32,
}

impl<N: DriverNotify> DescChain<N> {
    pub fn iter<'a, M: DriverMem>(&'a mut self, mem: &'a M) -> DescChainIter<'a, N, M> {
        DescChainIter {
            queue: self.queue.clone(),
            desc: Some(self.first_desc),
            chain: self,
            mem,
        }
    }
    pub fn set_written(&mut self, written: u32) {
        self.written = written;
    }
}

impl<N: DriverNotify> Drop for DescChain<N> {
    fn drop(&mut self) {
        self.queue.0.return_chain(self.first_desc, self.written);
    }
}
