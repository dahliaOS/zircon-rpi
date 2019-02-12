// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use itertools::structs::Product;
use itertools::Itertools;

use std::collections::HashMap;
use std::hash::Hash;
use std::num::NonZeroU16;
use std::option;

pub trait SocketAddress: Sized + Copy + Hash + Eq {
    // We can't use the IntoIterator trait (which also defines an iterator type
    // and an into_iter method) because we can't implement IntoIterator for
    // foreign types, which would mean we couldn't implement SocketAddress for
    // things like NonZeroU16.

    type Iter: Clone + Iterator<Item = Self>;

    const HAS_BUILTIN: bool;

    fn into_iter(self) -> Self::Iter;
    fn builtin(&self) -> bool;
}

pub trait PacketAddress: Copy {
    type SocketAddr: SocketAddress;

    fn into_socket_addr_incoming(self) -> Self::SocketAddr;
}

// This implementation for Option exists primarily to support UDP, whose remote
// packet port address type is Option<NonZeroU16>. It maps Some(addr) =>
// AllAddr::Addr(addr), and None => AllAddr::All. Note that one implication of
// this design choice is that a packet whose source port is not set can never be
// routed to a connection socket (since all connection sockets have fixed remote
// ports); they can only be routed to a listener socket.

impl<S: SocketAddress> PacketAddress for Option<S> {
    type SocketAddr = AllAddr<S>;

    fn into_socket_addr_incoming(self) -> AllAddr<S> {
        match self {
            Some(addr) => AllAddr::Addr(addr),
            None => AllAddr::All,
        }
    }
}

impl SocketAddress for ! {
    type Iter = option::IntoIter<!>;

    const HAS_BUILTIN: bool = false;

    fn into_iter(self) -> Self::Iter {
        Some(self).into_iter()
    }
    fn builtin(&self) -> bool {
        false
    }
}

impl_socket_address!(NonZeroU16, builtins => []);
impl_packet_address!(NonZeroU16);

#[derive(Copy, Clone, Hash, PartialEq, Eq)]
pub enum AllAddr<A> {
    Addr(A),
    All,
}

impl<A> AllAddr<A> {
    pub fn is_all(&self) -> bool {
        match self {
            AllAddr::All => true,
            _ => false,
        }
    }
}

impl<A: SocketAddress> SocketAddress for AllAddr<A> {
    type Iter = AllAddrIter<A>;

    const HAS_BUILTIN: bool = A::HAS_BUILTIN;

    fn into_iter(self) -> AllAddrIter<A> {
        AllAddrIter(Some(self))
    }

    fn builtin(&self) -> bool {
        match self {
            AllAddr::Addr(addr) => addr.builtin(),
            AllAddr::All => A::HAS_BUILTIN,
        }
    }
}

#[derive(Copy, Clone)]
pub struct AllAddrIter<A>(Option<AllAddr<A>>);

impl<A> Iterator for AllAddrIter<A> {
    type Item = AllAddr<A>;

    fn next(&mut self) -> Option<AllAddr<A>> {
        match self.0 {
            Some(AllAddr::Addr(_)) => std::mem::replace(&mut self.0, Some(AllAddr::All)),
            Some(AllAddr::All) => std::mem::replace(&mut self.0, None),
            None => None,
        }
    }
}

impl<A> From<A> for AllAddr<A> {
    fn from(a: A) -> AllAddr<A> {
        AllAddr::Addr(a)
    }
}

impl<A> Into<Option<A>> for AllAddr<A> {
    fn into(self) -> Option<A> {
        match self {
            AllAddr::Addr(addr) => Some(addr),
            AllAddr::All => None,
        }
    }
}

#[derive(Copy, Clone)]
pub enum AutoAddr<A> {
    Addr(A),
    Auto,
}

#[derive(Copy, Clone)]
pub enum AutoAllAddr<A> {
    Addr(A),
    Auto,
    All,
}

#[derive(Copy, Clone, Hash, PartialEq, Eq)]
pub struct ConnAddr<L, R> {
    local: L,
    remote: R,
}

impl<L, R> ConnAddr<L, R> {
    pub fn new(local: L, remote: R) -> ConnAddr<L, R> {
        ConnAddr { local, remote }
    }

    pub fn into_local_remote(self) -> (L, R) {
        let ConnAddr { local, remote } = self;
        (local, remote)
    }
}

impl<L: Copy, R> ConnAddr<L, R> {
    pub fn local(&self) -> L {
        self.local
    }
}

impl<L, R: Copy> ConnAddr<L, R> {
    pub fn remote(&self) -> R {
        self.remote
    }
}

impl<L: SocketAddress, R: SocketAddress> SocketAddress for ConnAddr<L, R> {
    type Iter = option::IntoIter<ConnAddr<L, R>>;

    const HAS_BUILTIN: bool = L::HAS_BUILTIN || R::HAS_BUILTIN;

    fn into_iter(self) -> option::IntoIter<ConnAddr<L, R>> {
        Some(self).into_iter()
    }

    fn builtin(&self) -> bool {
        self.local().builtin() || self.remote().builtin()
    }
}

impl<L, R> From<ConnAddr<L, R>> for ConnAddr<AllAddr<L>, AllAddr<R>> {
    fn from(addr: ConnAddr<L, R>) -> ConnAddr<AllAddr<L>, AllAddr<R>> {
        let (local, remote) = addr.into_local_remote();
        ConnAddr::new(local.into(), remote.into())
    }
}

impl<L, R> From<ConnAddr<AllAddr<L>, R>> for ConnAddr<AllAddr<L>, AllAddr<R>> {
    fn from(addr: ConnAddr<AllAddr<L>, R>) -> ConnAddr<AllAddr<L>, AllAddr<R>> {
        let (local, remote) = addr.into_local_remote();
        ConnAddr::new(local, remote.into())
    }
}

impl<L, R> From<ConnAddr<L, AllAddr<R>>> for ConnAddr<AllAddr<L>, AllAddr<R>> {
    fn from(addr: ConnAddr<L, AllAddr<R>>) -> ConnAddr<AllAddr<L>, AllAddr<R>> {
        let (local, remote) = addr.into_local_remote();
        ConnAddr::new(local.into(), remote)
    }
}

impl<L, R> Into<!> for ConnAddr<L, R> {
    fn into(self) -> ! {
        panic!("Into<!>::into")
    }
}

#[derive(Copy, Clone, Hash, PartialEq, Eq)]
pub struct PacketAddr<S, D> {
    src: S,
    dst: D,
}

impl<S, D> PacketAddr<S, D> {
    pub fn new(src: S, dst: D) -> PacketAddr<S, D> {
        PacketAddr { src, dst }
    }

    pub fn into_src_dst(self) -> (S, D) {
        let PacketAddr { src, dst } = self;
        (src, dst)
    }

    pub fn into_conn_incoming(self) -> ConnAddr<D, S> {
        let (src, dst) = self.into_src_dst();
        ConnAddr::new(dst, src)
    }
}

impl<S: Copy, D> PacketAddr<S, D> {
    pub fn src(&self) -> S {
        self.src
    }
}

impl<S, D: Copy> PacketAddr<S, D> {
    pub fn dst(&self) -> D {
        self.dst
    }
}

impl<S: PacketAddress, D: PacketAddress> PacketAddress for PacketAddr<S, D> {
    type SocketAddr = ConnAddr<D::SocketAddr, S::SocketAddr>;

    fn into_socket_addr_incoming(self) -> Self::SocketAddr {
        let (src, dst) = self.into_src_dst();
        ConnAddr::new(dst.into_socket_addr_incoming(), src.into_socket_addr_incoming())
    }
}

#[derive(Copy, Clone, Hash, PartialEq, Eq)]
pub struct AddrVec<H, R> {
    head: H,
    rest: R,
}

impl<H, R> AddrVec<H, R> {
    pub fn new(head: H, rest: R) -> AddrVec<H, R> {
        AddrVec { head, rest }
    }

    pub fn append<HH>(self, new_head: HH) -> AddrVec<HH, AddrVec<H, R>> {
        AddrVec { head: new_head, rest: self }
    }

    pub fn into_head_rest(self) -> (H, R) {
        let AddrVec { head, rest } = self;
        (head, rest)
    }
}

impl<H: Copy, R> AddrVec<H, R> {
    pub fn head(&self) -> H {
        self.head
    }
}

impl<H, R: Copy> AddrVec<H, R> {
    pub fn rest(&self) -> R {
        self.rest
    }
}

impl<H: SocketAddress, R: SocketAddress> SocketAddress for AddrVec<H, R> {
    type Iter = AddrVecIter<H, R>;

    const HAS_BUILTIN: bool = H::HAS_BUILTIN || R::HAS_BUILTIN;

    fn into_iter(self) -> AddrVecIter<H, R> {
        let (head, rest) = self.into_head_rest();
        AddrVecIter(head.into_iter().cartesian_product(rest.into_iter()))
    }

    fn builtin(&self) -> bool {
        self.head().builtin() || self.rest().builtin()
    }
}

impl<H: PacketAddress, R: PacketAddress> PacketAddress for AddrVec<H, R> {
    type SocketAddr = AddrVec<H::SocketAddr, R::SocketAddr>;

    fn into_socket_addr_incoming(self) -> Self::SocketAddr {
        let (head, rest) = self.into_head_rest();
        AddrVec::new(head.into_socket_addr_incoming(), rest.into_socket_addr_incoming())
    }
}

impl<H, R> From<(H, R)> for AddrVec<H, R> {
    fn from((head, rest): (H, R)) -> AddrVec<H, R> {
        AddrVec { head, rest }
    }
}

#[derive(Clone)]
pub struct AddrVecIter<H: SocketAddress, R: SocketAddress>(Product<H::Iter, R::Iter>);

impl<H: SocketAddress, R: SocketAddress> Iterator for AddrVecIter<H, R> {
    type Item = AddrVec<H, R>;

    fn next(&mut self) -> Option<AddrVec<H, R>> {
        self.0.next().map(AddrVec::from)
    }
}

struct SocketAddrMap<A, S> {
    map: HashMap<A, S>,
}

impl<A: SocketAddress, S> SocketAddrMap<A, S> {
    fn insert(&mut self, addr: A, socket: S) -> Result<(), (A, S)> {
        if addr.builtin() {
            return Err((addr, socket));
        }

        for addr in addr.into_iter() {
            if self.map.get(&addr).is_some() {
                return Err((addr, socket));
            }
        }

        let old = self.map.insert(addr, socket);
        debug_assert!(old.is_none());
        Ok(())
    }

    fn get_by_incoming_packet_addr<P: PacketAddress<SocketAddr = A>>(
        &self,
        addr: P,
    ) -> Option<(&A, &S)> {
        for addr in addr.into_socket_addr_incoming().into_iter() {
            if let Some((addr, socket)) = self.map.get_key_value(&addr) {
                return Some((addr, socket));
            }
        }

        None
    }

    pub fn retain<F>(&mut self, f: F)
    where
        F: FnMut(&A, &mut S) -> bool,
    {
        self.map.retain(f);
    }
}

// TODO(joshlf): Rename?
pub struct SocketMap<K, A, S> {
    _marker: std::marker::PhantomData<(K, A, S)>,
}

impl<K, A, S> Default for SocketMap<K, A, S> {
    fn default() -> SocketMap<K, A, S> {
        SocketMap { _marker: std::marker::PhantomData }
    }
}

impl<K, A: SocketAddress, S> SocketMap<K, A, S> {
    pub fn insert(&mut self, key: K, addr: A, socket: S) -> Result<(), (K, A, S)> {
        unimplemented!()
    }

    pub fn get_by_incoming_packet_addr<'s, 'a, P: PacketAddress<SocketAddr = A>>(
        &'s mut self,
        addr: &'a P,
    ) -> Option<(&'s K, &'a A, &'s mut S)> {
        unimplemented!()
    }

    pub fn get_by_key<'s, 'k>(&'s mut self, key: &'k K) -> Option<(&'k K, &'s A, &'s mut S)> {
        unimplemented!()
    }

    pub fn retain<F>(&mut self, f: F)
    where
        F: FnMut(&K, &A, &mut S) -> bool,
    {
        unimplemented!()
    }
}
