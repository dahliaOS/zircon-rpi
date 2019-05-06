// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, futures_api, integer_atomics, await_macro)]
#![allow(warnings)]

use {
    failure::{err_msg, format_err, Error, ResultExt},
    fidl::{endpoints::{self as endpoints, RequestStream, ServiceMarker}, encoding::OutOfLine},
    fidl_fuchsia_virtualization_hardware::{
        StartInfo, VirtioDeviceRequest, VirtioDeviceRequestStream, VirtioNetMarker,
        VirtioNetRequest, VirtioNetRequestStream, EVENT_SET_INTERRUPT, EVENT_SET_QUEUE,
    },
    fidl_fuchsia_hardware_ethernet::{self as ethernet},
    fidl_fuchsia_netstack::{self as netstack, NetstackProxy},
    fidl_fuchsia_net::{self as net},
    fuchsia_component::server,
    fuchsia_component::server::ServiceFs,
    fuchsia_async::{self as fasync, PacketReceiver, ReceiverRegistration, FifoReadable, FifoWritable},
    fuchsia_syslog::{self as syslog, fx_log_err, fx_log_info},
    fuchsia_zircon::{self as zx, AsHandleRef},
    futures::{Sink, SinkExt, channel::mpsc, select, task::{AtomicWaker, noop_waker_ref, noop_waker}, Future, Stream, StreamExt, TryFutureExt, FutureExt, TryStreamExt},
    parking_lot::Mutex,
    std::{
        collections::HashMap,
        mem,
        ops::{Deref, DerefMut},
        pin::Pin,
        sync::atomic,
        task::{Context, Poll},
        io::{Read, Write},
    },
    pin_utils::pin_mut,
    fuchsia_component::client::connect_to_service,
    zerocopy,
};

use machina_virtio_device::*;
use virtio_device::*;

struct GuestEthernet {
    rx_fifo: fasync::Fifo<EthernetFifoEntry>,
    tx_fifo: fasync::Fifo<EthernetFifoEntry>,
    vmo: zx::Vmo,
    io_mem: &'static [u8],
    con: fidl_fuchsia_hardware_ethernet::DeviceRequestStream,
}

const MTU: u32 = 1500;
// This is a locally administered MAC address (first byte 0x02) mixed with the
// Google Organizationally Unique Identifier (00:1a:11). The host gets ff:ff:ff
// and the guest gets 00:00:00 for the last three octets.
const HOST_MAC_ADDRESS: ethernet::MacAddress = ethernet::MacAddress{octets: [0x02, 0x1a, 0x11, 0xff, 0xff, 0xff]};

const VIRTIO_NET_QUEUE_SIZE: usize = 256;


// Copied from //zircon/system/public/zircon/device/ethernet.h
const ETH_FIFO_RX_OK: u16 = 1;   // packet received okay
const ETH_FIFO_TX_OK: u16 = 1;   // packet transmitted okay
const ETH_FIFO_INVALID: u16 = 2; // packet not within io_vmo bounds
const ETH_FIFO_RX_TX: u16 = 4;   // received our own tx packet (when TX_LISTEN)

#[repr(C, packed)]
pub struct EthernetFifoEntry {
  pub offset: u32,
  pub length: u16,
  pub flags: u16,
  pub cookie: u64,
}

unsafe impl fasync::FifoEntry for EthernetFifoEntry{}

impl GuestEthernet {
    async fn new(mut con: fidl_fuchsia_hardware_ethernet::DeviceRequestStream) -> Result<GuestEthernet, Error> {
        // start the message loop
        let mut fifo = None;
        let mut vmo = None;
        loop {
            match await!(con.next()) {
                Some(Ok(ethernet::DeviceRequest::GetInfo{responder})) => {
                    let mut info = ethernet::Info{features: ethernet::INFO_FEATURE_SYNTH, mtu: MTU,
                        mac: HOST_MAC_ADDRESS};
                    responder.send(&mut info);
                },
                Some(Ok(ethernet::DeviceRequest::GetFifos{responder})) => {
                    if fifo.is_some() {
                        panic!("Duplicated fifos");
                    }
                    // TODO: should send an error on failures instead of dropping responder?
                    let (rx_a, rx_b) = zx::Fifo::create(VIRTIO_NET_QUEUE_SIZE, mem::size_of::<EthernetFifoEntry>())?;
                    let (tx_a, tx_b) = zx::Fifo::create(VIRTIO_NET_QUEUE_SIZE, mem::size_of::<EthernetFifoEntry>())?;
                    let mut fifos = ethernet::Fifos {rx: rx_a, tx: tx_b, rx_depth: VIRTIO_NET_QUEUE_SIZE as u32, tx_depth: VIRTIO_NET_QUEUE_SIZE as u32};
                    let rx = fasync::Fifo::<EthernetFifoEntry>::from_fifo(rx_b)?;
                    let tx = fasync::Fifo::<EthernetFifoEntry>::from_fifo(tx_a)?;

                    responder.send(zx::Status::OK.into_raw(), Some(OutOfLine(&mut fifos)));
                    fifo = Some((tx, rx));
                },
                Some(Ok(ethernet::DeviceRequest::SetIoBuffer{h, responder})) => {
                    if vmo.is_some() {
                        panic!("Duplicated vmo");
                    }
                    let vmo_size = h.get_size()? as usize;
                    let addr = fuchsia_runtime::vmar_root_self().map(
                        0,
                        &h,
                        0,
                        vmo_size,
                        zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE | zx::VmarFlags::REQUIRE_NON_RESIZABLE,
                    )?;
                    responder.send(zx::Status::OK.into_raw());
                    vmo = Some((h, addr, vmo_size));
                },
                Some(Ok(ethernet::DeviceRequest::Start{responder})) => {
                    match (fifo, vmo) {
                        (Some((tx_fifo, rx_fifo)), Some((vmo, io_addr, io_size))) => {
                            responder.send(zx::Status::OK.into_raw());
                            let io_mem = unsafe{std::slice::from_raw_parts(io_addr as usize as *const u8, io_size)};
                            return Ok(GuestEthernet{tx_fifo, rx_fifo, vmo, io_mem, con});
                        },
                        _ => panic!("Start called too soon"),
                    }
                },
                Some(Ok(ethernet::DeviceRequest::SetClientName{name, responder})) => {
                    println!("Name set to {}", name);
                    responder.send(zx::Status::OK.into_raw());
                },
                Some(Ok(msg)) => return Err(format_err!("Unknown msg")),
                Some(Err(e)) => return Err(format_err!("Some fidl error")),
                None => return Err(format_err!("Unexpected end of stream")),
            }
        }
    }
    pub fn rx_packet_stream<'a>(&'a self) -> (PacketStream<'a, RxPacket>, PacketSink<'a, RxPacket>) {
        let mut stream = self.rx_fifo.buffered_stream(VIRTIO_NET_QUEUE_SIZE as usize / 2);
        (PacketStream { stream, io_mem: self.io_mem, packet_type: std::marker::PhantomData },
            PacketSink {fifo: &self.rx_fifo, packet_type: std::marker::PhantomData, pending: None})
    }
    pub fn tx_packet_stream<'a>(&'a self) -> (PacketStream<'a, TxPacket>, PacketSink<'a, TxPacket>) {
        let mut stream = self.tx_fifo.buffered_stream(VIRTIO_NET_QUEUE_SIZE as usize / 2);
        (PacketStream { stream, io_mem: self.io_mem, packet_type: std::marker::PhantomData },
            PacketSink {fifo: &self.tx_fifo, packet_type: std::marker::PhantomData, pending: None})
    }
}

struct PacketStream<'a, P> {
    stream: fasync::BufferedEntryStream<'a, fasync::Fifo<EthernetFifoEntry, EthernetFifoEntry>, EthernetFifoEntry>,
    io_mem: &'a [u8],
    packet_type: std::marker::PhantomData<P>,
}

impl<'a, P> Unpin for PacketStream<'a, P> {}

impl<'a, P: PacketFlags> Stream for PacketStream<'a, P> {
    type Item = Packet<'a, P>;

    fn poll_next(mut self: Pin<&mut Self>, lw: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        match self.stream.poll_next_unpin(lw) {
            Poll::Ready(Some(Err(e))) => {
                panic!("Cannot handle errors");
            },
            Poll::Ready(Some(Ok(mut entry))) => {
                let packet = self.io_mem.get(entry.offset as usize..entry.offset as usize + entry.length as usize)
                    .expect("netstack gave garbage packet address");
                let packet = unsafe{std::slice::from_raw_parts_mut(packet.as_ptr() as *mut u8, packet.len())};

                entry.flags = P::okay();
                Poll::Ready(Some(Packet{packet, entry, packet_type: std::marker::PhantomData}))
            },
            Poll::Ready(None) => Poll::Ready(None),
            Poll::Pending => Poll::Pending,
        }
    }
}

struct PacketSink<'a, P> {
    fifo: &'a fasync::Fifo<EthernetFifoEntry, EthernetFifoEntry>,
    packet_type: std::marker::PhantomData<P>,
    pending: Option<EthernetFifoEntry>,
}

impl<'a, P> Unpin for PacketSink<'a, P>{}

impl<'a, P> Sink<Packet<'a, P>> for PacketSink<'a, P> {
    type SinkError = zx::Status;

    fn poll_ready(self: Pin<&mut Self>, lw: &mut Context<'_>) -> Poll<Result<(), Self::SinkError>> {
        if self.pending.is_some() {
            // TODO: if poll flush completes successfully use poll_write
            self.poll_flush(lw)
        } else {
            // TODO: remove this203

            self.fifo.poll_write(lw).map(|r| r.map(|_| ()))
        }
        // TODO: work out how to do this here
        // self.fifo.poll_write(lw)
    }

    fn start_send(mut self: Pin<&mut Self>, item: Packet<'a, P>) -> Result<(), Self::SinkError> {
        self.pending = Some(item.entry);
        Ok(())
    }

    fn poll_flush(mut self: Pin<&mut Self>, lw: &mut Context<'_>) -> Poll<Result<(), Self::SinkError>> {
        if let Some(entry) = self.pending.as_ref() {
            match self.fifo.try_write(lw, std::slice::from_ref(entry)) {
                Poll::Ready(Ok(_)) => {
                    self.pending = None;
                    Poll::Ready(Ok(()))
                },
                Poll::Ready(Err(e)) => Poll::Ready(Err(e)),
                Poll::Pending => Poll::Pending,
            }
        } else {
            Poll::Ready(Ok(()))
        }
    }

    fn poll_close(self: Pin<&mut Self>, lw: &mut Context<'_>) -> Poll<Result<(), Self::SinkError>> {
        self.poll_flush(lw)
    }
}

trait PacketFlags {
    fn okay() -> u16;
    fn invalid() -> u16 {
        ETH_FIFO_INVALID
    }
}

struct RxPacket {}
struct TxPacket {}

impl PacketFlags for RxPacket {
    fn okay() -> u16 {
        ETH_FIFO_RX_OK
    }
}

impl PacketFlags for TxPacket {
    fn okay() -> u16 {
        ETH_FIFO_TX_OK
    }
}

struct Packet<'a, P> {
    packet: &'a[u8],
    entry: EthernetFifoEntry,
    packet_type: std::marker::PhantomData<P>,
}

impl<'a> Packet<'a, RxPacket> {
    pub fn set_length(self, len: usize) -> Result<Packet<'a, RxPacket>, Packet<'a, RxPacket>> {
        let Packet {packet, mut entry, packet_type} = self;
        match packet.get(0..len) {
            Some(packet) => {
                entry.length = len as u16;
                return Ok(Packet{packet, entry, packet_type})
            },
            None => Err(Packet{packet, entry, packet_type}),
        }
    }
}

impl<'a, P: PacketFlags> Packet<'a, P> {
    // TODO: mark result as must be used
    pub fn cancel(self) -> Packet<'a, P> {
        let Packet {packet, mut entry, packet_type} = self;
        let packet = &packet[0..0];
        entry.flags = P::invalid();
        Packet {packet, entry, packet_type}
    }
}

impl<'a, I: std::slice::SliceIndex<[u8]>> std::ops::IndexMut<I> for Packet<'a, RxPacket> {
    fn index_mut(&mut self, index: I) -> &mut I::Output {
        let packet = unsafe{std::slice::from_raw_parts_mut(self.packet.as_ptr() as *mut u8, self.packet.len())};
        packet.index_mut(index)
    }
}

impl<'a, I: std::slice::SliceIndex<[u8]>, P> std::ops::Index<I> for Packet<'a, P> {
    type Output = I::Output;
    fn index(&self, index: I) -> & I::Output {
        self.packet.index(index)
    }
}

const INTERFACE_PATH: &'static str = "/dev/class/ethernet/virtio";
const INTERFACE_NAME: &'static str = "ethv0";
const IPV4_ADDRESS: net::Ipv4Address = net::Ipv4Address{addr: [10, 0, 0, 1]};

#[repr(C, packed)]
#[derive(Debug, Clone, zerocopy::FromBytes, zerocopy::AsBytes)]
pub struct VirtioNetHdr {
    flags: u8,
    gso_type: u8,
    hdr_len: u16,
    gso_size: u16,
    csum_start: u16,
    csum_offset: u16,
    // Only if |VIRTIO_NET_F_MRG_RXBUF| or |VIRTIO_F_VERSION_1|.
    num_buffers: u16,
}

struct VirtioNetPacket<B> {
    header: zerocopy::LayoutVerified<B, VirtioNetHdr>,
    body: B,
}

#[repr(u16)]
enum NetQueues {
    RX = 0,
    TX = 1,
}

async fn run_virtio_net(mut con: VirtioNetRequestStream) -> Result<(), Error> {
    // Connect to the host nestack
    let netstack = connect_to_service::<netstack::NetstackMarker>().context("failed to connect to netstack")?;

    let mut con = DeviceStream::new(con);

    // Expect a start message first off to configure things.
    let (start_info, responder) =
        match await!(con.next()) {
            Some(Ok(VirtioNetRequest::Start { start_info, responder })) => {
                (start_info, responder)
            }
            _ => return Err(format_err!("Expected Start message.")),
        };

    let mut config = netstack::InterfaceConfig{name: INTERFACE_NAME.into(), metric: 0, ip_address_config:
        netstack::IpAddressConfig::StaticIp(net::Subnet{addr: net::IpAddress::Ipv4(IPV4_ADDRESS), prefix_len: 24})};

    let (device_client, device_server) = endpoints::create_endpoints::<ethernet::DeviceMarker>()?;
    let device_server = device_server.into_stream()?;

    let ethernet_start_future = GuestEthernet::new(device_server/*, INTERFACE_PATH, INTERFACE_NAME, IPV4_ADDRESS*/);

    // Add ourselves as an ethernet device to the netstack
    let add_device_future = async || {
        let id = await!(netstack.add_ethernet_device(INTERFACE_PATH, &mut config, device_client))?;
        netstack.set_interface_status(id, true)
    };
    let add_device_future = add_device_future();

    let device_future = DeviceBuilder::new(start_info, || responder.send())?
        .set_event(|e| Ok(BufferedNotify::new(NotifyEvent::new(e))))?
        .add_queue(NetQueues::RX as u16, true)?
        .add_queue(NetQueues::TX as u16, true)?
        .wait_for_ready(&mut con);

    let (guest_ethernet, (), (mut device, guest_mem, ready_responder)) =
        await!(futures::future::try_join3(ethernet_start_future, add_device_future.err_into::<Error>(),device_future))?;

    ready_responder.send()?;

    let mut tx_stream = device.take_stream(NetQueues::TX as u16)?;
    let mut rx_stream = device.take_stream(NetQueues::RX as u16)?;


    let rx_guest_mem = &guest_mem;
    let tx_guest_mem = &guest_mem;
    let (mut rx_packet_stream, mut rx_packet_sink) = guest_ethernet.rx_packet_stream();
    let (mut tx_packet_stream, mut tx_packet_sink) = guest_ethernet.tx_packet_stream();
    let notify = device.get_notify().clone();
    let rx_notify = notify.clone();
    let tx_notify = notify.clone();

    // Wait for some tx
    let rx_fut = async move || -> Result<(), Error> {
        let mut tx_stream = tx_stream.peekable();
        pin_mut!(tx_stream);
        let mut rx_packet_stream = rx_packet_stream.peekable();
        pin_mut!(rx_packet_stream);
        let mut rx_fifo_buf = Vec::new();
        while let Some(mut desc_chain) = await!(tx_stream.next()) {
            let mut chain = DescChainBytes::new(desc_chain.iter(tx_guest_mem))?;
            let header = chain.next_readable::<VirtioNetHdr>().unwrap();

            let packet_len = chain.readable_remaining();

            // TODO: should handle errors here, not propagate
            let packet = await!(rx_packet_stream.next()).unwrap();
            let packet = match packet.set_length(packet_len) {
                Ok(mut packet) => {
                    // The lengths are all correct, no reason this should fail
                    chain.read_exact(&mut packet[..]).unwrap();
                    // We should be at the end of the descriptor chain.
                    if !chain.end_of_readable() {
                        panic!("Invalid descriptor chain");
                    }
                    packet
                },
                Err(packet) => {
                    packet.cancel()
                }
            };
            rx_fifo_buf.push(packet);
            drop(chain);
            drop(desc_chain);
            // Attempt to guess if we will run another full iteration without blocking, and if so
            // delay the notification. This doesn't feel overly safe and should maybe be done
            // a better way.
            if tx_stream.as_mut().peek(&mut Context::from_waker(&noop_waker())).is_pending() ||
               rx_packet_stream.as_mut().peek(&mut Context::from_waker(&noop_waker())).is_pending() {
                let mut drain = futures::stream::iter(rx_fifo_buf.drain(..));
                await!(rx_packet_sink.send_all(&mut drain));
                rx_notify.flush();
            }
        }
        panic!("Descriptor stream shouldn't end");
        Ok(())
    }();
    let tx_fut = async move || -> Result<(), Error> {
        let mut tx_packet_stream = tx_packet_stream.peekable();
        pin_mut!(tx_packet_stream);
        let mut rx_stream =rx_stream.peekable();
        pin_mut!(rx_stream);
        let mut tx_fifo_buf = Vec::new();
        while let Some(mut packet) = await!(tx_packet_stream.next()) {
            let src_mem = &packet[..];
            // Need a descriptor now
            if let Some(mut desc_chain) = await!(rx_stream.next()) {
                let mut chain = DescChainBytes::new(desc_chain.iter(rx_guest_mem))?;
                let mut header = chain.next_written::<VirtioNetHdr>().unwrap();
                (*header)=VirtioNetHdr{
                    num_buffers: 1,
                    gso_type: 0,
                    gso_size: 0,
                    flags: 0,
                    hdr_len: 0,
                    csum_start: 0,
                    csum_offset: 0,};
                chain.write_all(src_mem).unwrap();
                tx_fifo_buf.push(packet);
            } else {
                panic!("Descriptor stream shouldn't end");
            }
            if tx_packet_stream.as_mut().peek(&mut Context::from_waker(noop_waker_ref())).is_pending() ||
               rx_stream.as_mut().peek(&mut Context::from_waker(noop_waker_ref())).is_pending(){
                let mut drain = futures::stream::iter(tx_fifo_buf.drain(..));
                await!(tx_packet_sink.send_all(&mut drain));
                tx_notify.flush();
            }
        }
        panic!("Tx Fifo closed");
        Ok(())
    }();

    // TODO; wait for messages on the guest ethernet con stream

    await!(futures::future::try_join3(device.err_into::<Error>(),rx_fut,tx_fut))?;

    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    syslog::init().expect("Unable to initialize syslog");
    let mut server = server::ServiceFs::new();
    server.dir("svc")
        .add_fidl_service(|stream: VirtioNetRequestStream| stream);
    server.take_and_serve_directory_handle().context("Error starting server")?;
    
    // We would like to use try_for_each_concurrent here but have not been able to make
    // a usage of that type check successfully.
    let serve_fut = server.for_each_concurrent(None, |stream| async {
        if let Err(e) = await!(run_virtio_net(stream)) {
            fx_log_err!("Error {} running virtio_net service", e);
        }
    });    

    await!(serve_fut);
    Ok(())
}
