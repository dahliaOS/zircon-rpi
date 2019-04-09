// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_mediacodec::*,
    fuchsia_syslog::fx_log_info,
    fuchsia_zircon::{self as zx, HandleBased},
    fidl_fuchsia_media::*,
    futures::{task::{Context, Poll, Waker}, stream::{FusedStream, Stream}, StreamExt, io::{self, AsyncWrite}, try_ready},
    failure::{Error, ResultExt, format_err},
    std::{collections::{HashMap, HashSet, VecDeque}, pin::Pin, sync::Arc},
    parking_lot::{Mutex, RwLock},
};


#[derive(Debug)]
enum Listener {
    /// No one is listening.
    None,
    /// Someone wants to listen but hasn't polled.
    New,
    /// Someone is listening, and can be woken whith the waker.
    Some(Waker),
}

impl Default for Listener {
    fn default() -> Self {
        Listener::None
    }
}

struct OutputQueue {
    listener: Listener,
    queue: VecDeque<Packet>,
}

impl Default for OutputQueue {
    fn default() -> Self {
        OutputQueue { listener: Listener::default(), queue: VecDeque::new() }
    }
}

struct EncoderInner {
    /// The proxy to the stream processor.
    processor: StreamProcessorProxy,
    /// The event stream from the StreamProcessor.  We handle these internally.
    events: StreamProcessorEventStream,
    /// Set of buffers that are used for input
    input_buffers: HashMap<u32, zx::Vmo>,
    /// The size of each input packet
    input_packet_size: u64,
    /// The set of input buffers that are available for writing by the client.
    client_owned: HashSet<u32>,
    /// A cursor on the next input buffer location to be written to when new input data arrives.
    input_cursor: Option<(u32, u64)>,
    /// Waker to be awoken when there is space in an input buffer.
    input_waker: Option<Waker>,
    /// The encoded data.
    /// The set of output buffers that will be written by the server.
    output_buffers: HashMap<u32, zx::Vmo>,
    /// The size of each output packet
    output_packet_size: u64,
    /// An queue of the indexes of output buffers that have been filled by the processor and a
    /// waiter if someone is waiting on it.
    output_queue: Mutex<OutputQueue>,
}

impl EncoderInner {
    fn handle_event(&mut self, evt: StreamProcessorEvent) -> Result<(), Error> {
        match evt {
        StreamProcessorEvent::OnInputConstraints { input_constraints: StreamBufferConstraints {
            default_settings, .. } } => {
                match default_settings {
                    None => fx_log_info!("OnInputConstraints with no default settings!"),
                    Some(mut settings) => {
                        let ordinal = 1;
                        settings.buffer_lifetime_ordinal = Some(ordinal);
                        self.input_packet_size = settings.per_packet_buffer_bytes.unwrap() as u64;
                        let packet_count = settings.packet_count_for_client.unwrap() +
                            settings.packet_count_for_server.unwrap();
                        self.processor.set_input_buffer_settings(settings)?;
                        for idx in 0..packet_count {
                            let (stream_buffer, vmo) = make_buffer(self.input_packet_size, ordinal, idx)?;
                            self.input_buffers.insert(idx, vmo);
                            self.client_owned.insert(idx);
                            self.processor.add_input_buffer(stream_buffer)?;
                        }
                        self.client_owned.take(&0).expect("should have owned the zero packet");
                        self.input_cursor = Some((0, 0));
                    }
                };
            }
        StreamProcessorEvent::OnOutputConstraints { output_config: StreamOutputConstraints {
            buffer_constraints_action_required, buffer_constraints, .. } }  => {
                if !buffer_constraints_action_required.expect("action required") {
                    return Ok(());
                }
                let constraints = buffer_constraints.expect("no constraints");
                let mut settings = constraints.default_settings.expect("no default_settings");
                let ordinal = 1;
                settings.buffer_lifetime_ordinal = Some(ordinal);
                let packet_count = settings.packet_count_for_client.unwrap() +
                    settings.packet_count_for_server.unwrap();
                self.output_packet_size = settings.per_packet_buffer_bytes.unwrap() as u64;
                self.processor.set_output_buffer_settings(settings)?;
                for idx in 0..packet_count {
                    // TODO(jamuraa): allocate one big vmo and chop it up for the packets.
                    let (stream_buffer, vmo) = make_buffer(self.output_packet_size, ordinal, idx)?;
                    self.output_buffers.insert(idx, vmo);
                    self.processor.add_output_buffer(stream_buffer)?;
                }
            }
        StreamProcessorEvent::OnOutputPacket { output_packet, .. } => {
                let mut lock = self.output_queue.lock();
                lock.queue.push_back(output_packet);
                if let Listener::Some(waker) = &lock.listener {
                    waker.wake_by_ref();
                }
            }
        StreamProcessorEvent::OnFreeInputPacket { free_input_packet: PacketHeader {
            buffer_lifetime_ordinal: Some(_ord), packet_index: Some(idx) } } => {
                self.client_owned.insert(idx);
            }
        e => fx_log_info!("Unhandled stream processor event: {:#?}", e),
        }
        Ok(())
    }
}

pub struct Encoder {
    inner: Arc<RwLock<EncoderInner>>,
}

fn make_buffer(bytes: u64, ordinal: u64, index: u32) -> Result<(StreamBuffer, zx::Vmo), Error> {
    let vmo = zx::Vmo::create(bytes)?;
    let vmo_copy = vmo.duplicate_handle(zx::Rights::SAME_RIGHTS)?;

    let data = StreamBufferData::Vmo(StreamBufferDataVmo {
        vmo_handle: Some(vmo),
        vmo_usable_start: Some(0),
        vmo_usable_size: Some(bytes),
    });
    Ok((StreamBuffer {
        buffer_lifetime_ordinal: Some(ordinal),
        buffer_index: Some(index),
        data: Some(data),
    }, vmo_copy))
}

pub struct EncodedStream {
    inner: Arc<RwLock<EncoderInner>>,
}

impl Encoder {
    pub fn start(params: CreateEncoderParams) -> Result<Encoder, Error> {
        let encoder_svc = fuchsia_component::client::connect_to_service::<CodecFactoryMarker>()
            .context("Failed to connect to Codec Factory")?;

        let (stream_processor_client, stream_processor_serverend) = fidl::endpoints::create_endpoints()?;
        let processor = stream_processor_client.into_proxy()?;

        encoder_svc.create_encoder(params, stream_processor_serverend)?;

        let events = processor.take_event_stream();

        let inner = Arc::new(RwLock::new(EncoderInner { processor, events, input_buffers: HashMap::new(),
        input_packet_size: 0, input_waker: None, client_owned: HashSet::new(), input_cursor: None,
            output_buffers: HashMap::new(),
            output_packet_size: 0,
            output_queue: Default::default(),
        }));
        Ok(Encoder { inner })
    }

    pub fn take_encoded_stream(&mut self) -> EncodedStream {
        {
            let read = self.inner.read();
            let mut lock = read.output_queue.lock();
            if let Listener::None = lock.listener {
                lock.listener = Listener::New;
            } else {
                panic!("Encoded output stream already taken");
            }
        }
        EncodedStream { inner: self.inner.clone() }
    }


    /// Deliver input to the encoder.  Returns the number of bytes delivered to the encoder.
    pub fn deliver_input(&mut self, bytes: &[u8]) -> Result<usize, io::Error> {
        let mut bytes_idx = 0;
        while bytes.len() > bytes_idx {
            {
                let mut write = self.inner.write();
                if write.input_cursor.is_none() {
                    return Ok(bytes_idx);
                }
                let (idx, size) = write.input_cursor.take().expect("input cursor is none");

                let space_left = write.input_packet_size - size;
                let left_to_write = bytes.len() - bytes_idx;
                let buffer_vmo = write.input_buffers.get_mut(&idx).expect("need buffer vmo");
                if space_left as usize >= left_to_write {
                    let write_buf = &bytes[bytes_idx..];
                    let write_len = write_buf.len();
                    buffer_vmo.write(write_buf, size)?;
                    bytes_idx += write_len;
                    write.input_cursor = Some((idx, size + write_len as u64));
                    assert!(bytes.len() == bytes_idx);
                    return Ok(bytes_idx);
                }
                let end_idx = bytes_idx + space_left as usize;
                let write_buf = &bytes[bytes_idx..end_idx];
                let write_len = write_buf.len();
                buffer_vmo.write(write_buf, size)?;
                bytes_idx += write_len;
                // this buffer is done, ship it!
                assert_eq!(size + write_len as u64, write.input_packet_size);
                write.input_cursor = Some((idx, write.input_packet_size));
            }
            self.flush_input_buf()?;
        }
        Ok(bytes_idx)
    }

    /// Flush the input buffer to the processor, relinquishing the ownership of the buffer
    /// currently in the input cursor, and picking a new input buffer.  If there is no input
    /// buffer left, the input cursor is left as None.
    pub fn flush_input_buf(&mut self) -> Result<(), io::Error> {
        let mut write = self.inner.write();
        if write.input_cursor.is_none() {
            // Nothing to flush
            return Ok(());
        }
        let (idx, size) = write.input_cursor.take().expect("input cursor is none");
        if size == 0 {
            // Can't send empty packet to processor.
            fx_log_info!("Nothing to flush: packet {} is empty", idx);
            write.input_cursor = Some((idx, size));
            return Ok(());
        }
        let packet = Packet {
            header: Some(PacketHeader {
                buffer_lifetime_ordinal: Some(1),
                packet_index: Some(idx),
            }),
            buffer_index: Some(idx),
            stream_lifetime_ordinal: Some(1),
            start_offset: Some(0),
            valid_length_bytes: Some(size as u32),
            timestamp_ish: None,
            start_access_unit: Some(true),
            known_end_access_unit: Some(true),
        };
        write.processor.queue_input_packet(packet).map_err(|e| io::Error::new(io::ErrorKind::Other, format_err!("Fidl Error: {}", e)))?;
        // pick another buffer.
        let next_idx = match write.client_owned.iter().cloned().next() {
            Some(idx) => idx,
            None => return Ok(()),
        };
        write.client_owned.take(&next_idx).unwrap();
        write.input_cursor = Some((next_idx, 0));
        Ok(())
    }

    /// Test whether it is possible to write to the Encoder. If there are no input buffers
    /// available, returns Poll::Pending and arranges for the current task to receive a
    /// notification when an input buffer becomes available.
    fn poll_writable(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let mut write  = self.inner.write();
        match &write.input_cursor {
            Some(_) => Poll::Ready(Ok(())),
            None => {
                write.input_waker = Some(cx.waker().clone());
                Poll::Pending
            }
        }
    }
}

impl AsyncWrite for Encoder {
    fn poll_write(mut self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &[u8]) -> Poll<io::Result<usize>> {
        try_ready!(self.poll_writable(cx));
        match self.deliver_input(buf) {
            Ok(written) => Poll::Ready(Ok(written)),
            Err(e) => Poll::Ready(Err(e.into())),
        }
    }

    fn poll_flush(mut self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(self.flush_input_buf())
    }

    fn poll_close(mut self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(self.flush_input_buf())
    }
}

impl Stream for EncodedStream {
    type Item = Result<Vec<u8>, Error>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        // Process the event stream if we can, or set a waker on it.
        let mut write = self.inner.write();
        let result = match StreamExt::poll_next_unpin(&mut write.events, cx) {
            Poll::Pending => Ok(()),
            Poll::Ready(Some(Ok(request))) => write.handle_event(request),
            Poll::Ready(Some(Err(e))) => Err(e.into()),
            Poll::Ready(None) => Ok(())
        };
        if let Err(e) = result {
            return Poll::Ready(Some(Err(e)));
        }
        let packet = {
            let mut lock = write.output_queue.lock();
            if lock.queue.is_empty() {
                lock.listener = Listener::Some(cx.waker().clone());
                return Poll::Pending;
            }
            lock.queue.pop_front().unwrap()
        };

        let header = packet.header.expect("need header");
        let output_size = packet.valid_length_bytes.expect("need valid length") as usize;
        let offset = packet.start_offset.expect("need offset") as u64;
        let mut output = vec![0; output_size];
        let buf_idx = packet.buffer_index.expect("need buffer index");
        let vmo = write.output_buffers.get_mut(&buf_idx).expect("need output vmo");
        if let Err(e) = vmo.read(&mut output, offset) {
            return Poll::Ready(Some(Err(e.into())));
        }
        if let Err(e) = write.processor.recycle_output_packet(header) {
            return Poll::Ready(Some(Err(e.into())));
        }
        Poll::Ready(Some(Ok(output)))
    }
}

impl FusedStream for EncodedStream {
    fn is_terminated(&self) -> bool {
        false
    }
}
