// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::address::{AddrVec, PacketAddress, SocketAddress, SocketMap};
use crate::error::NetstackError;
use crate::ip::{
    Ip, IpConnSocket, IpConnSocketAddr, IpConnSocketRequestAddr, IpDeviceConnSocket,
    IpDeviceConnSocketAddr, IpDeviceConnSocketRequestAddr, IpListenerSocket, IpListenerSocketAddr,
    IpListenerSocketRequestAddr, IpSocketAddr,
};

pub trait TransportSocketImpl {
    // TODO(joshlf): Change this to 'PacketAddr: PacketAddress'
    type Addr: SocketAddress;

    type ConnKey: Clone;
    type DeviceConnKey: Clone;
    type ListenerKey: Clone;

    type ConnAddr: SocketAddress + Into<Self::Addr>;
    type DeviceConnAddr: SocketAddress + Into<Self::Addr>;
    type ListenerAddr: SocketAddress + Into<Self::Addr>;

    type ConnSocket;
    type DeviceConnSocket;
    type ListenerSocket;
}

pub struct TransportSocket<Addr, Key, Sock, IpSock> {
    pub addr: Addr,
    pub key: Key,
    pub sock: Sock,
    pub ip_sock: IpSock,
}

pub enum Bar<I: Ip, T: TransportSocketImpl> {
    Conn(
        TransportSocket<
            TransportConnSocketAddr<T::ConnAddr, I::Addr>,
            T::ConnKey,
            T::ConnSocket,
            IpConnSocket<I>,
        >,
    ),
    DeviceConn(
        TransportSocket<
            TransportDeviceConnSocketAddr<T::DeviceConnAddr, I::Addr>,
            T::DeviceConnKey,
            T::DeviceConnSocket,
            IpDeviceConnSocket<I>,
        >,
    ),
    Listener(
        TransportSocket<
            TransportListenerSocketAddr<T::ListenerAddr, I::Addr>,
            T::ListenerKey,
            T::ListenerSocket,
            IpListenerSocket<I>,
        >,
    ),
}

impl<I: Ip, T: TransportSocketImpl> Bar<I, T> {
    fn unwrap_conn(
        &mut self,
    ) -> &mut TransportSocket<
        TransportConnSocketAddr<T::ConnAddr, I::Addr>,
        T::ConnKey,
        T::ConnSocket,
        IpConnSocket<I>,
    > {
        match self {
            Bar::Conn(c) => c,
            _ => panic!("Bar::unwrap_conn on non-Conn value"),
        }
    }

    fn unwrap_device_conn(
        &mut self,
    ) -> &mut TransportSocket<
        TransportDeviceConnSocketAddr<T::DeviceConnAddr, I::Addr>,
        T::DeviceConnKey,
        T::DeviceConnSocket,
        IpDeviceConnSocket<I>,
    > {
        match self {
            Bar::DeviceConn(c) => c,
            _ => panic!("Bar::unwrap_conn on non-DeviceConn value"),
        }
    }

    fn unwrap_listener(
        &mut self,
    ) -> &mut TransportSocket<
        TransportListenerSocketAddr<T::ListenerAddr, I::Addr>,
        T::ListenerKey,
        T::ListenerSocket,
        IpListenerSocket<I>,
    > {
        match self {
            Bar::Listener(l) => l,
            _ => panic!("Bar::unwrap_listener on non-Listener value"),
        }
    }
}

pub type TransportSocketAddr<T, A> = AddrVec<T, IpSocketAddr<A>>;

pub type TransportConnSocketAddr<T, A> = AddrVec<T, IpConnSocketAddr<A>>;

pub type TransportConnSocketRequestAddr<T, A> = AddrVec<T, IpConnSocketRequestAddr<A>>;

pub type TransportDeviceConnSocketAddr<T, A> = AddrVec<T, IpDeviceConnSocketAddr<A>>;

pub type TransportDeviceConnSocketRequestAddr<T, A> = AddrVec<T, IpDeviceConnSocketRequestAddr<A>>;

pub type TransportListenerSocketAddr<T, A> = AddrVec<T, IpListenerSocketAddr<A>>;

pub type TransportListenerSocketRequestAddr<T, A> = AddrVec<T, IpListenerSocketRequestAddr<A>>;

fn conn_addr_to_addr<I: Ip, T: TransportSocketImpl>(
    addr: TransportConnSocketAddr<T::ConnAddr, I::Addr>,
) -> TransportSocketAddr<T::Addr, I::Addr> {
    let (foo, ip) = addr.into_head_rest();
    AddrVec::new(foo.into(), ip.into())
}

fn device_conn_addr_to_addr<I: Ip, T: TransportSocketImpl>(
    addr: TransportDeviceConnSocketAddr<T::DeviceConnAddr, I::Addr>,
) -> TransportSocketAddr<T::Addr, I::Addr> {
    let (foo, ip) = addr.into_head_rest();
    AddrVec::new(foo.into(), ip.into())
}

fn listener_addr_to_addr<I: Ip, T: TransportSocketImpl>(
    addr: TransportListenerSocketAddr<T::ListenerAddr, I::Addr>,
) -> TransportSocketAddr<T::Addr, I::Addr> {
    let (foo, ip) = addr.into_head_rest();
    AddrVec::new(foo.into(), ip.into())
}

enum Key<T: TransportSocketImpl> {
    Conn(T::ConnKey),
    DeviceConn(T::DeviceConnKey),
    Listener(T::ListenerKey),
}

impl<T: TransportSocketImpl> Key<T> {
    fn unwrap_conn(&self) -> &T::ConnKey {
        match self {
            Key::Conn(c) => c,
            _ => panic!("Key::unwrap_conn on non-Conn value"),
        }
    }

    fn unwrap_device_conn(&self) -> &T::DeviceConnKey {
        match self {
            Key::DeviceConn(c) => c,
            _ => panic!("Key::unwrap_device_conn on non-DeviceConn value"),
        }
    }

    fn unwrap_listener(&self) -> &T::ListenerKey {
        match self {
            Key::Listener(l) => l,
            _ => panic!("Key::unwrap_listener on non-Listener value"),
        }
    }
}

pub struct TransportSocketMap<I: Ip, T: TransportSocketImpl> {
    map: SocketMap<Key<T>, TransportSocketAddr<T::Addr, I::Addr>, Bar<I, T>>,
}

impl<I: Ip, T: TransportSocketImpl> Default for TransportSocketMap<I, T> {
    fn default() -> TransportSocketMap<I, T> {
        TransportSocketMap { map: SocketMap::default() }
    }
}

impl<I: Ip, T: TransportSocketImpl> TransportSocketMap<I, T> {
    pub fn insert_conn(
        &mut self,
        key: T::ConnKey,
        addr: T::ConnAddr,
        sock: T::ConnSocket,
        ip_sock: IpConnSocket<I>,
    ) -> Result<(), NetstackError> {
        let addr = AddrVec::new(addr, ip_sock.addr());
        self.map
            .insert(
                Key::Conn(key.clone()),
                conn_addr_to_addr::<I, T>(addr.clone()),
                Bar::Conn(TransportSocket { addr, key, sock, ip_sock }),
            )
            .map_err(|_| unimplemented!())
    }

    pub fn insert_device_conn(
        &mut self,
        key: T::DeviceConnKey,
        addr: T::DeviceConnAddr,
        sock: T::DeviceConnSocket,
        ip_sock: IpDeviceConnSocket<I>,
    ) -> Result<(), NetstackError> {
        let addr = AddrVec::new(addr, ip_sock.addr());
        self.map
            .insert(
                Key::DeviceConn(key.clone()),
                device_conn_addr_to_addr::<I, T>(addr.clone()),
                Bar::DeviceConn(TransportSocket { addr, key, sock, ip_sock }),
            )
            .map_err(|_| unimplemented!())
    }

    pub fn insert_listener(
        &mut self,
        key: T::ListenerKey,
        addr: T::ListenerAddr,
        sock: T::ListenerSocket,
        ip_sock: IpListenerSocket<I>,
    ) -> Result<(), NetstackError> {
        let addr = AddrVec::new(addr, ip_sock.addr());
        self.map
            .insert(
                Key::Listener(key.clone()),
                listener_addr_to_addr::<I, T>(addr.clone()),
                Bar::Listener(TransportSocket { addr, key, sock, ip_sock }),
            )
            .map_err(|_| unimplemented!())
    }

    pub fn get_conn_by_key(
        &mut self,
        key: &T::ConnKey,
    ) -> Option<
        &mut TransportSocket<
            TransportConnSocketAddr<T::ConnAddr, I::Addr>,
            T::ConnKey,
            T::ConnSocket,
            IpConnSocket<I>,
        >,
    > {
        self.map.get_by_key(&Key::Conn(key.clone())).map(|(_key, _addr, sock)| sock.unwrap_conn())
    }

    pub fn get_device_conn_by_key(
        &mut self,
        key: &T::DeviceConnKey,
    ) -> Option<
        &mut TransportSocket<
            TransportDeviceConnSocketAddr<T::DeviceConnAddr, I::Addr>,
            T::DeviceConnKey,
            T::DeviceConnSocket,
            IpDeviceConnSocket<I>,
        >,
    > {
        self.map
            .get_by_key(&Key::DeviceConn(key.clone()))
            .map(|(_key, _addr, sock)| sock.unwrap_device_conn())
    }

    pub fn get_listener_by_key(
        &mut self,
        key: &T::ListenerKey,
    ) -> Option<
        &mut TransportSocket<
            TransportListenerSocketAddr<T::ListenerAddr, I::Addr>,
            T::ListenerKey,
            T::ListenerSocket,
            IpListenerSocket<I>,
        >,
    > {
        self.map
            .get_by_key(&Key::Listener(key.clone()))
            .map(|(_key, _addr, sock)| sock.unwrap_listener())
    }

    pub fn get_by_incoming_packet_addr<
        P: PacketAddress<SocketAddr = TransportSocketAddr<T::Addr, I::Addr>>,
    >(
        &mut self,
        addr: &P,
    ) -> Option<&mut Bar<I, T>> {
        self.map.get_by_incoming_packet_addr(addr).map(|(_key, _addr, sock)| sock)
    }

    pub fn retain<F>(&mut self, mut f: F)
    where
        F: FnMut(&mut Bar<I, T>) -> bool,
    {
        self.map.retain(|_key, _addr, socket| f(socket));
    }
}
