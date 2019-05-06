// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, futures_api, integer_atomics, await_macro)]
#![allow(warnings)]

use {
    failure::{err_msg, format_err, Error, ResultExt},
    fidl::endpoints::{RequestStream, ServiceMarker},
    fidl_fuchsia_virtualization_hardware::{
        StartInfo, VirtioDeviceRequest, VirtioDeviceRequestStream, VirtioRngMarker,
        VirtioRngRequest, VirtioRngRequestStream, EVENT_SET_INTERRUPT, EVENT_SET_QUEUE,
    },
    fuchsia_component::server::ServiceFs,
    fuchsia_async::{self as fasync, PacketReceiver, ReceiverRegistration},
    fuchsia_zircon::{self as zx, AsHandleRef},
    futures::{channel::mpsc, select, task::AtomicWaker, Future, FutureExt, Stream, StreamExt, TryFutureExt, stream},
    parking_lot::Mutex,
    std::{
        collections::HashMap,
        mem,
        ops::{Deref, DerefMut},
        pin::Pin,
        sync::atomic,
        task::{Context, Poll},
    },
};

use virtio_device::*;

pub struct PortPacketStreamReceiver {
    channel: mpsc::UnboundedSender<zx::Packet>,
}

impl PacketReceiver for PortPacketStreamReceiver {
    fn receive_packet(&self, packet: zx::Packet) {
        self.channel.unbounded_send(packet);
    }
}

// TODO: this is generic
pub struct PortPacketStream {
    receiver: ReceiverRegistration<PortPacketStreamReceiver>,
    channel: mpsc::UnboundedReceiver<zx::Packet>,
}

impl PortPacketStream {
    pub fn new() -> Self {
        let (tx, rx) = mpsc::unbounded();
        let receiver = fasync::EHandle::local()
            .register_receiver(std::sync::Arc::new(PortPacketStreamReceiver { channel: tx }));
        PortPacketStream { receiver, channel: rx }
    }
}

impl Stream for PortPacketStream {
    type Item = zx::Packet;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.channel.poll_next_unpin(cx)
    }
}

pub struct GuestBellTrap(PortPacketStream);

impl GuestBellTrap {
    pub fn new(guest: &zx::Guest, addr: usize, len: usize) -> Result<Self, zx::Status> {
        let receiver = PortPacketStream::new();
        guest.set_trap_bell(
            zx::GPAddr(addr),
            len,
            receiver.receiver.port(),
            receiver.receiver.key(),
        )?;
        Ok(GuestBellTrap(receiver))
    }
}

impl Stream for GuestBellTrap {
    type Item = zx::GuestBellPacket;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.0.poll_next_unpin(cx).map(|packet| match packet.map(|p| p.contents()) {
            Some(zx::PacketContents::GuestBell(bell)) => Some(bell),
            Some(x) => panic!("Registered for GUEST_BELL but received something else: {:?}", x),
            None => None,
        })
    }
}

// TODO: use VirtioDeviceStream
pub struct QueueNotificationStream {
    trap: Option<GuestBellTrap>,
    con: VirtioDeviceRequestStream,
}

impl QueueNotificationStream {
    pub fn new(
        trap: Option<GuestBellTrap>,
        con: VirtioDeviceRequestStream,
    ) -> QueueNotificationStream {
        QueueNotificationStream { trap, con }
    }
}

pub enum QNError {
    Fidl(fidl::Error),
    Message(VirtioDeviceRequest),
}

impl std::fmt::Debug for QNError {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            QNError::Fidl(e) => write!(f, "QNError::Fidl({:?})",e),
            QNError::Message(_) => write!(f, "QNError::Message(...)"),
        }
    }
}

impl Stream for QueueNotificationStream {
    type Item = Result<u16, QNError>;

    fn poll_next(mut self: Pin<&mut Self>, lw: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        if let Some(mut bell) = self.trap.as_mut() {
            match bell.poll_next_unpin(lw) {
                // TODO: calculate queue number
                Poll::Ready(Some(packet)) => {
                    return Poll::Ready(Some(Ok(0)));
                }
                Poll::Ready(None) => {
                    return Poll::Ready(None);
                }
                _ => {}
            }
        }
        match self.con.poll_next_unpin(lw) {
            Poll::Ready(Some(Ok(VirtioDeviceRequest::NotifyQueue {
                queue: queue_num,
                control_handle: _,
            }))) => Poll::Ready(Some(Ok(queue_num))),

            Poll::Ready(Some(Ok(m))) => Poll::Ready(Some(Err(QNError::Message(m)))),
            Poll::Ready(Some(Err(e))) => Poll::Ready(Some(Err(QNError::Fidl(e)))),
            Poll::Ready(None) => Poll::Ready(None),
            Poll::Pending => Poll::Pending,
        }
    }
}

#[derive(Fail, Debug)]
pub enum QNRError {
    #[fail(display = "Error in stream")]
    Stream(QNError),
    #[fail(display = "Unknown queue")]
    UnknownQueue(u16),
}

// TODO: this is really just a hash waker and could be generic
pub struct QueueNotificationRunner {
    wakers: HashMap<u16, std::sync::Arc<AtomicWaker>>,
    notify_stream: QueueNotificationStream,
}

impl QueueNotificationRunner {
    pub fn from_stream(notify_stream: QueueNotificationStream) -> QueueNotificationRunner {
        QueueNotificationRunner { wakers: HashMap::new(), notify_stream }
    }
    pub fn new(
        trap: Option<GuestBellTrap>,
        con: VirtioDeviceRequestStream,
    ) -> QueueNotificationRunner {
        Self::from_stream(QueueNotificationStream::new(trap, con))
    }
    pub fn add_queue<S, N>(&mut self, queue_index: u16, queue_stream: &S) -> bool
    where
        S: Deref<Target = DescChainStream<N>>,
    {
        self.wakers.insert(queue_index, queue_stream.waker()).is_none()
    }
}

impl Future for QueueNotificationRunner {
    type Output = Result<(), QNRError>;

    fn poll(mut self: Pin<&mut Self>, lw: &mut Context<'_>) -> Poll<Self::Output> {
        loop {
            match self.notify_stream.poll_next_unpin(lw) {
                Poll::Ready(None) => return Poll::Ready(Ok(())),
                Poll::Ready(Some(Err(e))) => return Poll::Ready(Err(QNRError::Stream(e))),
                Poll::Ready(Some(Ok(queue))) => {
                    if let Some(waker) = self.wakers.get_mut(&queue) {
                        waker.wake();
                    } else {
                        return Poll::Ready(Err(QNRError::UnknownQueue(queue)));
                    }
                }
                Poll::Pending => return Poll::Pending,
            }
        }
    }
}

// TODO: this is generic
pub const USER_SIGNALS: [zx::Signals; 8] = [
    zx::Signals::USER_0,
    zx::Signals::USER_1,
    zx::Signals::USER_2,
    zx::Signals::USER_3,
    zx::Signals::USER_4,
    zx::Signals::USER_5,
    zx::Signals::USER_6,
    zx::Signals::USER_7,
];

// We want to view the RNG device as a fill sink and not an item sink. This means
// the descriptors should be filled with items, and not one item sent per descriptor.

struct NotifyEventInner {
    event: zx::Event,
}

#[repr(transparent)]
#[derive(Clone)]
pub struct NotifyEvent(std::sync::Arc<NotifyEventInner>);

impl NotifyEvent {
    pub fn new(event: zx::Event) -> NotifyEvent {
        NotifyEvent(std::sync::Arc::new(NotifyEventInner{event}))
    }
}

impl DriverNotify for NotifyEvent {
    fn notify(&self) {
        self.0.event
            .as_handle_ref()
            .signal(
                zx::Signals::empty(),
                USER_SIGNALS[EVENT_SET_QUEUE as usize] | USER_SIGNALS[EVENT_SET_INTERRUPT as usize],
            )
            .unwrap();
    }
}

pub struct GuestMem {
    mem: &'static [u8],
}

impl GuestMem {
    pub fn from_vmo(vmo: zx::Vmo) -> Result<GuestMem, Error> {
        let vmo_size = vmo.get_size()? as usize;
        let addr = fuchsia_runtime::vmar_root_self().map(
            0,
            &vmo,
            0,
            vmo_size,
            zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE,
        )?;
        // Guest memory is mapped in the range addr..vmo_size
        let mem = unsafe { std::slice::from_raw_parts_mut(addr as *mut u8, vmo_size) };
        Ok(GuestMem { mem })
    }
    pub fn translate_const(&self, addr: DriverAddr, len: u32) -> Option<&'static [u8]> {
        self.mem.get(addr.0..addr.0 + len as usize).map(|slice| unsafe {
            std::slice::from_raw_parts(slice.as_ptr() as *const u8, slice.len())
        })
    }
    pub fn translate_rings(
        &self,
        size: u16,
        desc: DriverAddr,
        avail: DriverAddr,
        used: DriverAddr,
    ) -> Option<(&'static [u8], &'static [u8], &'static mut [u8])> {
        let desc_len = std::mem::size_of::<ring::Desc>() * size as usize;
        let avail_len = ring::Driver::avail_len_for_count(size as u16);
        let used_len = ring::Device::used_len_for_count(size as u16);
        let desc = self.translate_const(desc, desc_len as u32)?;
        let avail = self.translate_const(avail, avail_len as u32)?;
        let used = self.translate(used, used_len as u32)?;
        // TODO: should sanity check none of the rings overlap
        Some((desc, avail, used))
    }
}

impl DriverMem for GuestMem {
    fn translate(&self, addr: DriverAddr, len: u32) -> Option<&'static mut [u8]> {
        self.mem.get(addr.0..addr.0 + len as usize).map(|slice| unsafe {
            std::slice::from_raw_parts_mut(slice.as_ptr() as *mut u8, slice.len())
        })
    }
}

pub struct DeviceStream<T> {
    stream: Option<T>,
}

impl<T> DeviceStream<T> {
    pub fn new(stream: T) -> DeviceStream<T> {
        DeviceStream { stream: Some(stream) }
    }
}

pub struct QueueConfig {
    queue: u16,
    size: u16,
    desc: DriverAddr,
    avail: DriverAddr,
    used: DriverAddr,
    responder: fidl_fuchsia_virtualization_hardware::VirtioDeviceConfigureQueueResponder,
}

pub enum DeviceRecv {
    Queue(QueueConfig),
    Ready(u32, fidl_fuchsia_virtualization_hardware::VirtioDeviceReadyResponder),
}

use failure::Fail;

#[derive(Fail, Debug)]
pub enum DeviceStreamError {
    #[fail(display = "Unexpected message.")]
    UnexpectedMessage,
    #[fail(
        display = "Error in decoding FIDL message. Could be unexpected device specific message."
    )]
    FidlError(#[fail(cause)] fidl::Error),
    #[fail(display = "Unexpected stream end whilst waiting for message.")]
    EndOfStream,
}

impl<T: fidl::endpoints::RequestStream> DeviceStream<T> {
    pub async fn recv_queue(&mut self) -> Result<QueueConfig, DeviceStreamError> {
        match await!(self.recv_queue_or_ready()) {
            Ok(DeviceRecv::Ready(_, _)) => Err(DeviceStreamError::UnexpectedMessage),
            Ok(DeviceRecv::Queue(config)) => Ok(config),
            Err(e) => Err(e),
        }
    }
    pub async fn recv_ready(
        &mut self,
    ) -> Result<(u32, fidl_fuchsia_virtualization_hardware::VirtioDeviceReadyResponder), DeviceStreamError>
    {
        match await!(self.recv_queue_or_ready()) {
            Ok(DeviceRecv::Ready(features, responder)) => Ok((features, responder)),
            Ok(DeviceRecv::Queue(_)) => Err(DeviceStreamError::UnexpectedMessage),
            Err(e) => Err(e),
        }
    }
    pub async fn recv_queue_or_ready(&mut self) -> Result<DeviceRecv, DeviceStreamError> {
        let mut stream: VirtioDeviceRequestStream = self.stream.take().unwrap().cast_stream();

        let result = match await!(stream.next()) {
            Some(Ok(VirtioDeviceRequest::ConfigureQueue {
                queue,
                size,
                desc,
                avail,
                used,
                responder,
            })) => Ok(DeviceRecv::Queue(QueueConfig {
                queue,
                size,
                desc: DriverAddr(desc as usize),
                avail: DriverAddr(avail as usize),
                used: DriverAddr(used as usize),
                responder,
            })),
            Some(Ok(VirtioDeviceRequest::Ready { negotiated_features, responder })) => {
                Ok(DeviceRecv::Ready(negotiated_features, responder))
            }
            Some(Ok(_)) => Err(DeviceStreamError::UnexpectedMessage),
            Some(Err(e)) => Err(DeviceStreamError::FidlError(e)),
            None => Err(DeviceStreamError::EndOfStream),
        };

        self.stream = Some(stream.cast_stream());
        result
    }
}

impl<T> Deref for DeviceStream<T> {
    type Target = T;

    fn deref(&self) -> &T {
        self.stream.as_ref().unwrap()
    }
}

impl<T> DerefMut for DeviceStream<T> {
    fn deref_mut(&mut self) -> &mut T {
        self.stream.as_mut().unwrap()
    }
}

impl<T: fidl::endpoints::RequestStream> From<DeviceStream<T>> for VirtioDeviceRequestStream {
    fn from(stream: DeviceStream<T>) -> VirtioDeviceRequestStream {
        stream.stream.unwrap().cast_stream()
    }
}

pub struct Device<N> {
    notify: N,
    queues: HashMap<u16, Queue<N>>,
    streams: HashMap<u16, DescChainStream<N>>,
    notify_runner: QueueNotificationRunner,
}

impl<N> Unpin for Device<N>{}

impl<N> Device<N> {
    pub fn take_stream(&mut self, queue: u16) -> Result<DescChainStream<N>, Error> {
        self.streams.remove(&queue).ok_or(format_err!("invalid queue"))
    }
    pub fn get_notify(&self) -> &N {
        &self.notify
    }
}

impl<N> Future for Device<N> {
    type Output = Result<(), QNRError>;

    fn poll(mut self: Pin<&mut Self>, lw: &mut Context<'_>) -> Poll<Self::Output> {
        self.notify_runner.poll_unpin(lw)
    }
}

pub struct DeviceBuilder<N> {
    notify: Option<N>,
    event: Option<zx::Event>,
    mem: GuestMem,
    trap: Option<GuestBellTrap>,
    queues: HashMap<u16, bool>,
}

pub struct NoEventMarker;

impl DeviceBuilder<NoEventMarker> {
    // TODO: coerece error?
    pub fn new<E: Into<Error>>(info: StartInfo, on_success_responder: impl FnOnce() -> Result<(), E>) -> Result<DeviceBuilder<NoEventMarker>, Error> {
        let StartInfo {trap, guest, event, vmo} = info;
        let mem = GuestMem::from_vmo(vmo)?;
        let trap = match guest {
            Some(guest) => Some(GuestBellTrap::new(&guest, trap.addr as usize, trap.size as usize)?),
            None => None,
        };

        on_success_responder().map_err(|e| e.into())?;
        Ok(DeviceBuilder { notify: None, event: Some(event), mem, trap, queues: HashMap::new()})
    }
    pub fn set_event<T: DriverNotify + Clone>(self, event_maker: impl FnOnce(zx::Event)->Result<T, Error>) -> Result<DeviceBuilder<T>, Error> {
        let DeviceBuilder {
            notify,
            mut event,
            mem,
            trap,
            queues} = self;
        let notify = Some(event_maker(event.take().unwrap())?);
        Ok(DeviceBuilder{notify, event, mem, trap, queues})
    }
}

impl<N> DeviceBuilder<N> {
    pub fn add_queue(mut self, queue: u16, required: bool) -> Result<DeviceBuilder<N>, Error> {
        match self.queues.insert(queue, required) {
            None => Ok(self),
            Some(_) => Err(format_err!("Duplicate")),
        }
    }
}

impl<'a, T: 'a + Clone> DeviceBuilder<T> {
    pub async fn wait_for_ready<I: fidl::endpoints::RequestStream>(self, con: &'a mut DeviceStream<I>) -> Result<(Device<T>, GuestMem, fidl_fuchsia_virtualization_hardware::VirtioDeviceReadyResponder), Error> {
        let mut configured_queues = HashMap::new();
        let DeviceBuilder {
            mut notify,
            event,
            mem,
            trap,
            queues
        } = self;
        let notify = notify.take().unwrap();
        while let msg = await!(con.recv_queue_or_ready())? {
            match msg {
                DeviceRecv::Queue(QueueConfig {
                    queue,
                    size,
                    desc,
                    avail,
                    used,
                    responder,
                }) => {
                    let (desc, avail, used) = mem.translate_rings(size, desc, avail, used)
                        .ok_or(format_err!("invalid guest memory"))?;
                    let q = Queue::new(desc, avail, used, notify.clone())?;
                    if let Some(_) = configured_queues.insert(queue, q) {
                        return Err(format_err!("Duplicate queue"));
                    }
                    responder.send()?;
                },
                DeviceRecv::Ready(features, responder) => {
                    // TODO: send the features to the queue
                    // TODO: validate we configured all the required queues

                    let mut notify_runner = QueueNotificationRunner::new(trap, con.stream.take().unwrap().cast_stream());
                    let mut desc_streams = HashMap::new();
                    for (k, q) in configured_queues.iter() {
                        let desc_stream = DescChainStream::new(q.clone());
                        // TODO: this is whack
                        notify_runner.add_queue(*k, &&desc_stream);
                        desc_streams.insert(*k, desc_stream);
                    }
                    let device = Device {notify, queues: configured_queues, notify_runner, streams: desc_streams };
                    return Ok((device, mem, responder))
                },
            };
        }
        Err(format_err!("Failed to receive Ready"))
    }
}

impl DeviceBuilder<NoEventMarker> {
    pub async fn wait_for_ready<I: fidl::endpoints::RequestStream>(self, con: &mut DeviceStream<I>) -> Result<(Device<NotifyEvent>, GuestMem, fidl_fuchsia_virtualization_hardware::VirtioDeviceReadyResponder), Error> {
        await!(self.set_event(|e| Ok(NotifyEvent::new(e)))?.wait_for_ready(con))
    }
}
