// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(dead_code)]

use {
    bitflags::bitflags,
    fuchsia_async as fasync,
    fuchsia_syslog::{fx_log_info, fx_log_warn},
    fuchsia_zircon::{self as zx, MessageBuf},
    fidl_fuchsia_media,
    futures::{
        ready, select,
        stream::{FusedStream, Stream},
        task::{Context, Poll, Waker},
        Future, StreamExt,
    },
    parking_lot::Mutex,
    std::{collections::VecDeque, mem, pin::Pin, sync::Arc},
};

use crate::types::{AudioSampleFormat, Decodable, Error, MaybeStream, Result};

mod ring_buffer;
mod stream;
mod encoder;

pub use encoder::Encoder;

#[repr(C)]
#[derive(Debug, Clone)]
struct AudioCommandHeader {
    transaction_id: u32,
    command_type: u32,
}

const AUDIO_CMD_HEADER_LEN: usize = mem::size_of::<AudioCommandHeader>();

transmute_decodable!(AudioCommandHeader);
transmute_intovec!(AudioCommandHeader);

#[derive(Debug)]
struct ChannelResponder<T: Decodable> {
    channel: Option<Arc<ChannelInner<T>>>,
    transaction_id: u32,
    command_type: u32,
}

impl<T: Decodable> ChannelResponder<T> {
    fn build<U>(transaction_id: u32, command_type: &U) -> ChannelResponder<T> where U: Into<u32> + Clone  {
        ChannelResponder { transaction_id, command_type: command_type.clone().into(), channel: None }
    }

    fn set_channel(&mut self, channel: Arc<ChannelInner<T>>) {
        self.channel = Some(channel);
    }

    fn make_header(&self) -> AudioCommandHeader {
        AudioCommandHeader {
            transaction_id: self.transaction_id,
            command_type: self.command_type,
        }
    }

    fn send(&self, payload: &[u8]) -> Result<()> {
        self.send_with_handles(payload, vec![])
    }

    fn send_with_handles(&self, payload: &[u8], handles: Vec<zx::Handle>) -> Result<()> {
        if self.channel.is_none() {
            return Err(Error::NoChannel);
        }
        self.channel.as_ref().unwrap().send(self.make_header(), payload, handles)
    }
}

bitflags! {
    #[repr(transparent)]
    #[derive(Default)]
    struct AudioStreamRangeFlags: u16 {
        const FPS_CONTINUOUS   = 0b001;
        const FPS_48000_FAMILY = 0b010;
        const FPS_44100_FAMILY = 0b100;
    }
}

#[repr(C)]
#[derive(Default, Clone)]
pub(crate) struct AudioStreamFormatRange {
    sample_formats: u32,
    min_frames_per_second: u32,
    max_frames_per_second: u32,
    min_channels: u8,
    max_channels: u8,
    flags: AudioStreamRangeFlags,
}

#[derive(Debug)]
struct RequestQueue<T> {
    listener: RequestListener,
    queue: VecDeque<T>,
}

impl<T> Default for RequestQueue<T> {
    fn default() -> Self {
        RequestQueue { listener: RequestListener::default(), queue: VecDeque::<T>::new() }
    }
}

#[derive(Debug)]
enum RequestListener {
    /// No one is listening.
    None,
    /// Someone wants to listen but hasn't polled.
    New,
    /// Someone is listening, and can be woken whith the waker.
    Some(Waker),
}

impl Default for RequestListener {
    fn default() -> Self {
        RequestListener::None
    }
}

#[derive(Debug)]
pub(crate) struct ChannelInner<T: Decodable> {
    /// The Channel
    channel: fasync::Channel,

    /// A request queue for the channel
    queue: Mutex<RequestQueue<T>>,
}

impl<T: Decodable> ChannelInner<T> {
    fn new(channel: zx::Channel) -> Result<ChannelInner<T>> {
        let chan = fasync::Channel::from_channel(channel)
            .or_else(|_| Err(Error::IOError(zx::Status::IO)))?;
        Ok(ChannelInner { channel: chan, queue: Mutex::<RequestQueue<T>>::default() })
    }

    fn take_request_stream(s: Arc<Self>) -> RequestStream<T> {
        {
            let mut lock = s.queue.lock();
            if let RequestListener::None = lock.listener {
                lock.listener = RequestListener::New;
            } else {
                panic!("Request stream has already been taken");
            }
        }

        RequestStream { inner: s }
    }

    // Attempts to receive a new request by processing all packets on the socket.
    // Resolves to a request T if one was received or an error if there was
    // an error reading from the socket.
    fn poll_recv_request(&self, cx: &mut Context<'_>) -> Poll<Result<T>>
    where
        T: Decodable,
    {
        let _ = self.recv_all(cx)?;

        let mut lock = self.queue.lock();
        if let Some(request) = lock.queue.pop_front() {
            Poll::Ready(Ok(request))
        } else {
            lock.listener = RequestListener::Some(cx.waker().clone());
            Poll::Pending
        }
    }

    fn recv_all(&self, cx: &mut Context<'_>) -> Result<()>
    where
        T: Decodable,
    {
        let mut buf = MessageBuf::new();
        loop {
            match self.channel.recv_from(cx, &mut buf) {
                Poll::Ready(Err(e)) => return Err(Error::PeerRead(e)),
                Poll::Pending => return Ok(()),
                Poll::Ready(Ok(())) => (),
            };
            let request = T::decode(buf.bytes())?;
            let mut lock = self.queue.lock();
            lock.queue.push_back(request);
        }
    }

    fn send(
        &self,
        header: AudioCommandHeader,
        params: &[u8],
        mut handles: Vec<zx::Handle>,
    ) -> Result<()> {
        let mut packet: Vec<u8> = header.into();
        packet.extend_from_slice(params);
        self.channel.as_ref().write(&packet, &mut handles).map_err(|x| Error::PeerWrite(x))?;
        Ok(())
    }
}

struct RequestStream<T: Decodable> {
    inner: Arc<ChannelInner<T>>,
}

impl<T: Decodable> Unpin for RequestStream<T> {}

impl<T: Decodable> Drop for RequestStream<T> {
    fn drop(&mut self) {
        self.inner.queue.lock().listener = RequestListener::None;
    }
}

impl<T: Decodable> FusedStream for RequestStream<T> {
    fn is_terminated(&self) -> bool {
        false
    }
}

impl Stream for RequestStream<stream::Request> {
    type Item = Result<stream::Request>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        Poll::Ready(match ready!(self.inner.poll_recv_request(cx)) {
            Ok(mut x) => {
                x.set_responder_channel(self.inner.clone());
                Some(Ok(x))
            }
            Err(e) => Some(Err(e)),
        })
    }
}

impl Stream for RequestStream<ring_buffer::Request> {
    type Item = Result<ring_buffer::Request>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        Poll::Ready(match ready!(self.inner.poll_recv_request(cx)) {
            Ok(mut x) => {
                x.set_responder_channel(self.inner.clone());
                Some(Ok(x))
            }
            Err(e) => Some(Err(e)),
        })
    }
}

/// A Stream that produces audio frames.
/// Usually acquired via SoftPdmAudioOutput::take_frame_stream().
// TODO: return the time that the first frame is meant to be presented?
pub struct AudioFrameStream {
    /// The VMO that is receiving the frames.
    frame_vmo: Arc<Mutex<ring_buffer::FrameVmo>>,
    /// A timer to set the waiter to poll when there are no frames available.
    timer: fasync::Timer,
    /// The last time we received frames.
    /// None if it hasn't started yet.
    last_frame_time: Option<zx::Time>,
    /// Minimum slice of time to deliver audio data in.
    /// This must be longer than the duration of one frame.
    min_packet_duration: zx::Duration,
}

impl AudioFrameStream {
    fn new(frame_vmo: Arc<Mutex<ring_buffer::FrameVmo>>, min_packet_duration: zx::Duration) -> AudioFrameStream {
        AudioFrameStream {
            frame_vmo,
            timer: fasync::Timer::new(zx::Time::INFINITE_PAST),
            last_frame_time: Some(zx::Time::get(zx::ClockId::Monotonic)),
            min_packet_duration,
        }
    }
}

impl Stream for AudioFrameStream {
    type Item = Result<Vec<u8>>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let now = zx::Time::get(zx::ClockId::Monotonic);
        if self.last_frame_time.is_none() {
            self.last_frame_time = Some(now);
        }
        let from = self.last_frame_time.take().expect("need last frame time");
        let next_frame_time = {
            let mut lock = self.frame_vmo.lock();
            match lock.next_frame_after(from) {
                Err(Error::InvalidState) => {
                    lock.set_start_waker(cx.waker().clone());
                    return Poll::Pending;
                }
                Err(e) => return Poll::Ready(Some(Err(e))),
                Ok(time) => time,
            }
        };
        if next_frame_time > now {
            self.last_frame_time = Some(from);
            self.timer = fasync::Timer::new(from + self.min_packet_duration);
            let poll = fasync::Timer::poll(Pin::new(&mut self.timer), cx);
            assert!(poll == Poll::Pending);
            return Poll::Pending;
        }
        let res = self.frame_vmo.lock().get_frames(from, now);
        match res {
            Ok((frames, missed)) => {
                if missed > 0 {
                    fx_log_info!("Missed {} frames due to slow polling", missed);
                }
                self.last_frame_time = Some(now);
                Poll::Ready(Some(Ok(frames)))
            }
            Err(e) => Poll::Ready(Some(Err(e))),
        }
    }
}

impl FusedStream for AudioFrameStream {
    fn is_terminated(&self) -> bool {
        false
    }
}

/// A software fuchsia audio output, which implements Audio Driver Streaming Interface
/// as defined in //zircon/docs/driver_interfaces/audio.md
pub struct SoftPcmAudioOutput {
    /// The Stream channel handles format negotiation, plug detection, and gain
    stream: Arc<ChannelInner<stream::Request>>,

    /// The RingBuffer command channel handles audio buffers and buffer notifications
    rb_chan: Option<Arc<ChannelInner<ring_buffer::Request>>>,

    /// The Unique ID that this stream will present to the system
    unique_id: [u8; 16],
    /// The manufacturer of the hardware for this stream
    manufacturer: String,
    /// A product description for the hardware for the stream
    product: String,

    /// The currently set format, in frames per second, audio sample format, and channels.
    supported_format: AudioStreamFormatRange,

    /// The currently set format, in frames per second, audio sample format, and channels.
    current_format: Option<(u32, AudioSampleFormat, u16)>,

    /// The request stream for the ringbuffer.
    rb_requests: MaybeStream<RequestStream<ring_buffer::Request>>,

    /// A pointer to the ring buffer for this stream
    frame_vmo: Arc<Mutex<ring_buffer::FrameVmo>>,
}

impl SoftPcmAudioOutput {
    /// Create a new Audio Stream, returning a client socket to be given to AudioCore.
    /// Spawns a task to process the events on the stream.
    pub fn build(
        unique_id: &[u8; 16],
        manufacturer: &str,
        product: &str,
        pcm_format: fidl_fuchsia_media::PcmFormat,
        min_packet_duration: zx::Duration,
    ) -> Result<(zx::Channel, AudioFrameStream)> {
        let (client_channel, stream_channel) =
            zx::Channel::create().or_else(|e| Err(Error::IOError(e)))?;

        // TODO: support more bit formats.
        let supported_sample_format = match pcm_format.bits_per_sample {
            16 => AudioSampleFormat::Sixteen { unsigned: false, invert_endian: false },
            _ => return Err(Error::OutOfRange),
        };

        let channels = pcm_format.channel_map.len() as u8;
        let supported_format = AudioStreamFormatRange {
            sample_formats: (&supported_sample_format).into(),
            min_frames_per_second: pcm_format.frames_per_second,
            max_frames_per_second: pcm_format.frames_per_second,
            min_channels: channels,
            max_channels: channels,
            flags: AudioStreamRangeFlags::FPS_CONTINUOUS,
        };

        let stream = SoftPcmAudioOutput {
            stream: Arc::new(ChannelInner::new(stream_channel)?),
            rb_chan: None,
            unique_id: unique_id.clone(),
            manufacturer: manufacturer.to_string(),
            product: product.to_string(),
            supported_format,
            current_format: None,
            rb_requests: Default::default(),
            frame_vmo: Arc::new(Mutex::new(ring_buffer::FrameVmo::new()?)),
        };

        let rb = stream.frame_vmo.clone();
        fuchsia_async::spawn(stream.process_events());
        Ok((client_channel, AudioFrameStream::new(rb, min_packet_duration)))
    }


    fn take_stream_requests(&self) -> RequestStream<stream::Request> {
        fx_log_warn!("Taking the stream requests");
        ChannelInner::take_request_stream(self.stream.clone())
    }

    fn take_ringbuffer_requests(&self) -> RequestStream<ring_buffer::Request> {
        fx_log_warn!("Taking the ringbuffer requests");
        let s = self
            .rb_chan
            .as_ref()
            .expect("Can't take the ringbuffer requests without a ringbuffer")
            .clone();
        ChannelInner::take_request_stream(s)
    }


    fn handle_control_request(&mut self, request: stream::Request) -> Result<()> {
        match request {
            stream::Request::GetFormats { responder } => {
                fx_log_info!("GetFormats");
                responder.reply(vec![self.supported_format.clone()])
            }
            stream::Request::SetFormat { responder, frames_per_second, sample_format, channels } => {
                match self.set_format(frames_per_second, sample_format, channels) {
                    Ok(channel) => {
                        self.rb_requests.set(self.take_ringbuffer_requests());
                        responder.reply(zx::Status::OK, 0, Some(channel))
                    }
                    Err(e) => responder.reply(zx::Status::NOT_SUPPORTED, 0, None).and(Err(e))
                }
            }
            stream::Request::GetGain { responder } => {
                // TODO: A way to set the gain
                responder.reply(None, None, 0.0, [0.0, 0.0], 0.0)
            }
            stream::Request::PlugDetect { responder, .. } => {
                // Always plugged in
                responder.reply(true, false, zx::Time::get(zx::ClockId::Monotonic))
            }
            stream::Request::GetUniqueId { responder } => {
                responder.reply(&self.unique_id)
            }
            stream::Request::GetString { id, responder } => {
                let s = match id {
                    stream::StringId::Manufacturer => &self.manufacturer,
                    stream::StringId::Product => &self.product,
                };
                responder.reply(s)
            }
            request => {
                fx_log_info!("Unimplemented audio control request: {:?}", request);
                Ok(())
            }
        }
    }

    fn handle_ring_buffer_request(&self, request: ring_buffer::Request) -> Result<()> {
        match request {
            ring_buffer::Request::GetFifoDepth { responder } => {
                // TODO: configurable
                responder.reply(zx::Status::OK, 32768)
            }
            ring_buffer::Request::GetBuffer { min_ring_buffer_frames, notifications_per_ring: _, responder } => {
                let (fps, format, channels) = self.current_format.clone().expect("ring_buffer get without format set");
                match self.frame_vmo.lock().set_format(fps, format, channels, min_ring_buffer_frames as usize) {
                    Err(e) => {
                        let audio_err = match e {
                            Error::IOError(zx_err) => zx_err,
                            _ => zx::Status::BAD_STATE,
                        };
                        responder.reply(audio_err, 0, None).and(Err(e))
                    },
                    Ok(vmo_handle) =>
                        responder.reply(zx::Status::OK, min_ring_buffer_frames, Some(vmo_handle.into())),
                }
            }
            ring_buffer::Request::Start { responder } => {
                let time = zx::Time::get(zx::ClockId::Monotonic);
                match self.frame_vmo.lock().start(time) {
                    Err(_) => responder.reply(zx::Status::BAD_STATE, 0),
                    Ok(_) => responder.reply(zx::Status::OK, time.into_nanos() as u64),
                }
            }
            ring_buffer::Request::Stop { responder } => {
                self.frame_vmo.lock().stop();
                responder.reply(zx::Status::OK)
            }
        }

    }

    async fn process_events(mut self) {
        fx_log_warn!("Starting to process events");
        let mut requests: RequestStream<stream::Request> = self.take_stream_requests();

        loop {
            let mut rb_request_fut = self.rb_requests.next();
            select! {
                request = requests.next() => {
                    let res = match request {
                        None => Err(Error::PeerRead(zx::Status::UNAVAILABLE)),
                        Some(Err(e)) => Err(e),
                        Some(Ok(r)) => self.handle_control_request(r),
                    };
                    if let Err(e) = res {
                        fx_log_warn!("Audio Control Error: {:?}, stopping", e);
                        return;
                    }
                }
                rb_request = rb_request_fut => {
                    let res = match rb_request {
                        None => Err(Error::PeerRead(zx::Status::UNAVAILABLE)),
                        Some(Err(e)) => Err(e),
                        Some(Ok(r)) => self.handle_ring_buffer_request(r),
                    };
                    if let Err(e) = res {
                        fx_log_warn!("Ring Buffer Error: {:?}, stopping", e);
                        return;
                    }
                }
                complete => break,
            }
        }

        fx_log_info!("All Streams ended, stopping processing...");
    }

    fn set_format(
        &mut self,
        frames_per_second: u32,
        sample_format: AudioSampleFormat,
        channels: u16,
    ) -> Result<zx::Channel> {
        // TODO: validate against the self.supported_sample_format
        self.current_format = Some((frames_per_second, sample_format, channels));
        let (client_rb_channel, rb_channel) =
            zx::Channel::create().or_else(|e| Err(Error::IOError(e)))?;
        self.rb_chan = Some(Arc::new(ChannelInner::new(rb_channel)?));
        Ok(client_rb_channel)
    }
}
