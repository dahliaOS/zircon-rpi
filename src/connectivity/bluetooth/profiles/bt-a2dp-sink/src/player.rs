// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _, Error},
    async_utils::hanging_get::client::HangingGetStream,
    bitfield::bitfield,
    bt_a2dp::codec::MediaCodecConfig,
    bt_avdtp::{MediaCodecType, RtpHeader},
    fidl_fuchsia_media::{
        AudioConsumerProxy, AudioConsumerStartFlags, AudioConsumerStatus, AudioSampleFormat,
        AudioStreamType, Compression, SessionAudioConsumerFactoryMarker,
        SessionAudioConsumerFactoryProxy, StreamPacket, StreamSinkProxy, NO_TIMESTAMP,
        STREAM_PACKET_FLAG_DISCONTINUITY,
    },
    fuchsia_async as fasync,
    fuchsia_audio_codec::StreamProcessor,
    fuchsia_syslog::fx_log_info,
    fuchsia_trace as trace,
    fuchsia_zircon::{self as zx, HandleBased},
    futures::{
        channel::mpsc,
        future::{AbortHandle, Abortable, Aborted},
        io::{AsyncWrite, AsyncWriteExt},
        task::{Context, Poll},
        Future, StreamExt,
    },
    std::{convert::TryInto, io, pin::Pin},
};

use crate::latm::AudioMuxElement;
use crate::DEFAULT_SAMPLE_RATE;

const DEFAULT_BUFFER_LEN: usize = 65536;

struct AudioConsumerSink {
    buffer: zx::Vmo,
    buffer_len: usize,
    current_offset: usize,
    tx_count: u64,
    first_packet_sent: Option<fasync::Time>,
    flags_receiver: mpsc::Receiver<u32>,
    stream_sink: StreamSinkProxy,
    audio_consumer: AudioConsumerProxy,
}

impl AudioConsumerSink {
    fn build(
        audio_consumer: &mut AudioConsumerProxy,
        frames_per_second: u32,
        mut compression: Option<Compression>,
        flags_receiver: mpsc::Receiver<u32>,
    ) -> Result<AudioConsumerSink, Error> {
        let (stream_sink, stream_sink_server) = fidl::endpoints::create_proxy()?;

        let mut audio_stream_type = AudioStreamType {
            sample_format: AudioSampleFormat::Signed16,
            channels: 2, // Stereo
            frames_per_second,
        };

        let buffer_len = DEFAULT_BUFFER_LEN;

        let buffer = zx::Vmo::create(buffer_len as u64)?;
        let buffers = vec![buffer.duplicate_handle(
            zx::Rights::READ
                | zx::Rights::DUPLICATE
                | zx::Rights::GET_PROPERTY
                | zx::Rights::TRANSFER
                | zx::Rights::MAP,
        )?];

        audio_consumer.create_stream_sink(
            &mut buffers.into_iter(),
            &mut audio_stream_type,
            compression.as_mut(),
            stream_sink_server,
        )?;

        Ok(AudioConsumerSink {
            buffer,
            buffer_len,
            current_offset: 0,
            tx_count: 0,
            first_packet_sent: None,
            flags_receiver,
            stream_sink,
            audio_consumer: audio_consumer.clone(),
        })
    }

    /// Push an encoded media frame into the buffer and signal that it's there to media.
    fn send_frame(&mut self, frame: &[u8], flags: u32) -> Result<(), Error> {
        if frame.len() > self.buffer_len {
            self.stream_sink.end_of_stream()?;
            return Err(format_err!("frame is too large for buffer"));
        }

        trace::duration!("bt-a2dp-sink", "Media:PacketSent");

        self.tx_count += 1;
        trace::flow_begin!("stream-sink", "SendPacket", self.tx_count);

        if self.current_offset + frame.len() > self.buffer_len {
            self.current_offset = 0;
        }

        let start_offset = self.current_offset;

        self.buffer.write(frame, self.current_offset as u64)?;
        let mut packet = StreamPacket {
            pts: NO_TIMESTAMP,
            payload_buffer_id: 0,
            payload_offset: start_offset as u64,
            payload_size: frame.len() as u64,
            buffer_config: 0,
            flags,
            stream_segment_id: 0,
        };

        if self.first_packet_sent.is_none() {
            let now = fasync::Time::now();
            self.first_packet_sent = Some(now);
            self.audio_consumer.start(AudioConsumerStartFlags::SupplyDriven, 0, NO_TIMESTAMP)?;
        }
        self.stream_sink.send_packet_no_reply(&mut packet)?;

        self.current_offset += frame.len();
        Ok(())
    }
}

impl AsyncWrite for AudioConsumerSink {
    fn poll_write(
        mut self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        // TODO(48584): check to see if we have a free buffer here
        let mut flags = 0;
        loop {
            match self.flags_receiver.try_next() {
                Ok(Some(flag)) => flags |= flag,
                Ok(None) | Err(_) => break,
            }
        }
        match self.send_frame(buf, flags) {
            Ok(()) => Poll::Ready(Ok(buf.len())),
            Err(e) => Poll::Ready(Err(io::Error::new(io::ErrorKind::Other, e))),
        }
    }

    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        // TODO(48584): Track the flushing and return when this ie empty
        Poll::Ready(Ok(()))
    }

    fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        // TODO(48584): Actually close the stream.
        Poll::Ready(Ok(()))
    }
}

#[derive(Debug)]
pub enum PlayerEvent {
    Closed,
    Status(AudioConsumerStatus),
}

#[derive(Debug, PartialEq)]
enum ChannelMode {
    Mono,
    DualChannel,
    Stereo,
    JointStereo,
}

impl From<u8> for ChannelMode {
    fn from(bits: u8) -> Self {
        match bits {
            0 => ChannelMode::Mono,
            1 => ChannelMode::DualChannel,
            2 => ChannelMode::Stereo,
            3 => ChannelMode::JointStereo,
            _ => panic!("invalid channel mode"),
        }
    }
}

bitfield! {
    pub struct SbcHeader(u32);
    impl Debug;
    u8;
    syncword, _: 7, 0;
    subbands, _: 8;
    allocation_method, _: 9;
    into ChannelMode, channel_mode, _: 11, 10;
    blocks_bits, _: 13, 12;
    frequency_bits, _: 15, 14;
    bitpool_bits, _: 23, 16;
    crccheck, _: 31, 24;
}

impl SbcHeader {
    /// The number of channels, based on the channel mode in the header.
    /// From Table 12.18 in the A2DP Spec.
    fn channels(&self) -> usize {
        match self.channel_mode() {
            ChannelMode::Mono => 1,
            _ => 2,
        }
    }

    fn has_syncword(&self) -> bool {
        const SBC_SYNCWORD: u8 = 0x9c;
        self.syncword() == SBC_SYNCWORD
    }

    /// The number of blocks, based on tbe bits in the header.
    /// From Table 12.17 in the A2DP Spec.
    fn blocks(&self) -> usize {
        4 * (self.blocks_bits() + 1) as usize
    }

    fn bitpool(&self) -> usize {
        self.bitpool_bits() as usize
    }

    /// Number of subbands based on the header bit.
    /// From Table 12.20 in the A2DP Spec.
    fn num_subbands(&self) -> usize {
        if self.subbands() {
            8
        } else {
            4
        }
    }

    /// Calculates the frame length.
    /// Formula from Section 12.9 of the A2DP Spec.
    fn frame_length(&self) -> Result<usize, Error> {
        if !self.has_syncword() {
            return Err(format_err!("syncword does not match"));
        }
        let len = 4 + (4 * self.num_subbands() * self.channels()) / 8;
        let rest = (match self.channel_mode() {
            ChannelMode::Mono | ChannelMode::DualChannel => {
                self.blocks() * self.channels() * self.bitpool()
            }
            ChannelMode::Stereo => self.blocks() * self.bitpool(),
            ChannelMode::JointStereo => self.num_subbands() + (self.blocks() * self.bitpool()),
        } as f64
            / 8.0)
            .ceil() as usize;
        Ok(len + rest)
    }
}

/// Players are configured and accept media frames, which are sent to the
/// media subsystem.
pub struct Player {
    codec_config: MediaCodecConfig,
    audio_sink: Pin<Box<dyn AsyncWrite>>,
    watch_status_stream: HangingGetStream<AudioConsumerStatus>,
    playing: bool,
    next_packet_flags: mpsc::Sender<u32>,
    last_seq_played: u16,
    decoder_task: Option<AbortHandle>,
}

impl Player {
    /// Attempt to make a new player that decodes and plays frames encoded in the
    /// `codec`
    pub fn new(session_id: u64, codec_config: MediaCodecConfig) -> Result<Player, Error> {
        let audio_consumer_factory =
            fuchsia_component::client::connect_to_service::<SessionAudioConsumerFactoryMarker>()
                .context("Failed to connect to audio consumer factory")?;
        Self::from_proxy(session_id, codec_config, audio_consumer_factory)
    }

    /// Build a AudioConsumer given a SessionAudioConsumerFactoryProxy.
    /// Used in tests.
    pub(crate) fn from_proxy(
        session_id: u64,
        codec_config: MediaCodecConfig,
        audio_consumer_factory: SessionAudioConsumerFactoryProxy,
    ) -> Result<Player, Error> {
        let mut decoder = None;
        let encoding = codec_config.stream_encoding();
        let mut compression = Some(Compression { type_: encoding.to_string(), parameters: None });
        let mut decoder_task = None;
        if codec_config.codec_type() == &MediaCodecType::AUDIO_SBC {
            let dec = StreamProcessor::create_decoder(
                codec_config.mime_type(),
                Some(codec_config.codec_extra().to_vec()),
            )?;
            compression = None;
            decoder = Some(dec);
        }

        let (mut audio_consumer, audio_consumer_server) = fidl::endpoints::create_proxy()?;

        audio_consumer_factory.create_audio_consumer(session_id, audio_consumer_server)?;

        let (sender, receiver) = mpsc::channel(1);

        let mut audio_sink: Pin<Box<dyn AsyncWrite>> = Box::pin(AudioConsumerSink::build(
            &mut audio_consumer,
            codec_config.sampling_frequency().unwrap_or(DEFAULT_SAMPLE_RATE),
            compression,
            receiver,
        )?);

        if let Some(mut decoder) = decoder {
            let mut decoded_stream = decoder.take_output_stream()?;
            let mut sink = audio_sink;
            let decoding_task_fut = async move {
                while let Some(decoded) = decoded_stream.next().await {
                    let decoded = match decoded {
                        Ok(decoded) => decoded,
                        Err(e) => {
                            fx_log_info!("Decoded stream failed to produce: {:?}", e);
                            break;
                        }
                    };
                    if let Err(e) = sink.write_all(&decoded).await {
                        fx_log_info!("AudioConsumer failed to write: {:?}", e);
                        break;
                    }
                }
            };
            audio_sink = Box::pin(decoder);
            let (stop_handle, stop_registration) = AbortHandle::new_pair();
            let abortable_task_fut = Abortable::new(decoding_task_fut, stop_registration);
            fuchsia_async::spawn_local(async move {
                if let Err(Aborted) = abortable_task_fut.await {
                    fx_log_info!("Decoder forwarding task completed.");
                }
            });
            decoder_task = Some(stop_handle);
        }

        let watch_status_stream =
            HangingGetStream::new(Box::new(move || Some(audio_consumer.watch_status())));

        Ok(Player {
            codec_config,
            audio_sink,
            watch_status_stream,
            playing: false,
            next_packet_flags: sender,
            last_seq_played: 0,
            decoder_task,
        })
    }

    /// Test that a given codec `config` is playable.
    /// If an error occurs, playing audio via any Player with the same `codec_config` is likely to
    /// fail.
    ///
    /// It also tests that a decoder can be found if the AudioConsumer provided by the system
    /// cannot decode the compressed format as specified in the `config`.
    /// Communicates with the current default AudioConsumer.
    pub async fn test_playable(config: &MediaCodecConfig) -> Result<(), Error> {
        let audio_consumer_factory =
            fuchsia_component::client::connect_to_service::<SessionAudioConsumerFactoryMarker>()
                .context("Failed to connect to audio consumer factory")?;
        let mut player = Self::from_proxy(0, config.clone(), audio_consumer_factory)?;

        // wait for initial event
        match player.next_event().await {
            PlayerEvent::Closed => Err(format_err!("AudioConsumer closed")),
            PlayerEvent::Status(_status) => Ok(()),
        }
    }

    /// Given a buffer with an SBC frame at the start, find the length of the
    /// SBC frame.
    fn find_sbc_frame_len(buf: &[u8]) -> Result<usize, Error> {
        if buf.len() < 4 {
            return Err(format_err!("Buffer too short for header"));
        }
        SbcHeader(u32::from_le_bytes(buf[0..4].try_into()?)).frame_length()
    }

    /// Accepts a payload which may contain multiple frames and breaks it into
    /// frames and sends it to media.
    pub async fn push_payload(&mut self, payload: &[u8]) -> Result<(), Error> {
        trace::duration_begin!("bt-a2dp-sink", "Media:PacketReceived");
        let rtp = RtpHeader::new(payload)?;

        let seq = rtp.sequence_number();
        let discontinuity = seq.wrapping_sub(self.last_seq_played.wrapping_add(1));

        self.last_seq_played = seq;

        if discontinuity > 0 && self.playing {
            let _ = self.next_packet_flags.try_send(STREAM_PACKET_FLAG_DISCONTINUITY);
        };

        let mut offset = RtpHeader::LENGTH;

        // TODO(40918) Handle SBC packet header
        offset += self.codec_config.rtp_frame_header().len();

        while offset < payload.len() {
            match self.codec_config.codec_type() {
                &MediaCodecType::AUDIO_SBC => {
                    let len = Player::find_sbc_frame_len(&payload[offset..]).or_else(|e| {
                        let _ = self.next_packet_flags.try_send(STREAM_PACKET_FLAG_DISCONTINUITY);
                        Err(e)
                    })?;
                    trace::instant!("bt-a2dp-sink", "SBC frame", trace::Scope::Thread);
                    if offset + len > payload.len() {
                        let _ = self.next_packet_flags.try_send(STREAM_PACKET_FLAG_DISCONTINUITY);
                        return Err(format_err!("Ran out of buffer for SBC frame"));
                    }

                    if let Err(e) = self.audio_sink.write_all(&payload[offset..offset + len]).await
                    {
                        fx_log_info!("Failed to push packet to audio: {:?}", e);
                    }
                    offset += len;
                }
                &MediaCodecType::AUDIO_AAC => {
                    let element = AudioMuxElement::try_from_bytes(&payload[offset..])?;

                    let frame = element.get_payload(0).ok_or(format_err!("Payload not found"))?;
                    if let Err(e) = self.audio_sink.write_all(frame).await {
                        fx_log_info!("Failed to write packet to sink: {:?}", e);
                    }
                    // Only one payload per AAC RTP Pakcet.
                    break;
                }
                _ => return Err(format_err!("Unrecognized codec!")),
            }
        }
        if let Err(e) = self.audio_sink.flush().await {
            fx_log_info!("Failed to flush audio packets: {:?}", e);
        }
        self.playing = true;
        trace::duration_end!("bt-a2dp-sink", "Media:PacketReceived");
        Ok(())
    }

    /// If PlayerEvent::Closed is returned, that indicates the underlying
    /// service went away and the player should be closed/rebuilt
    ///
    /// This function should be always be polled when running
    pub fn next_event(&mut self) -> impl Future<Output = PlayerEvent> + '_ {
        let next_fut = self.watch_status_stream.next();
        async move {
            match next_fut.await {
                None => PlayerEvent::Closed,
                Some(Err(_)) => PlayerEvent::Closed,
                Some(Ok(s)) => PlayerEvent::Status(s),
            }
        }
    }
}

impl Drop for Player {
    fn drop(&mut self) {
        self.decoder_task.take().map(|t| t.abort());
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use matches::assert_matches;

    use {
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_media::{
            AudioConsumerRequest, AudioConsumerRequestStream, SessionAudioConsumerFactoryRequest,
            SessionAudioConsumerFactoryRequestStream, StreamSinkRequest, StreamSinkRequestStream,
        },
        fuchsia_async as fasync,
        futures::{pin_mut, task::Poll},
    };

    #[test]
    fn test_frame_length() {
        // 44.1, 16 blocks, Joint Stereo, Loudness, 8 subbands, 53 bitpool (Android P)
        let header1 = [0x9c, 0xBD, 0x35, 0xA2];
        const HEADER1_FRAMELEN: usize = 119;
        let head = SbcHeader(u32::from_le_bytes(header1));
        assert!(head.has_syncword());
        assert_eq!(16, head.blocks());
        assert_eq!(ChannelMode::JointStereo, head.channel_mode());
        assert_eq!(2, head.channels());
        assert_eq!(53, head.bitpool());
        assert_eq!(HEADER1_FRAMELEN, head.frame_length().unwrap());
        assert_eq!(
            HEADER1_FRAMELEN,
            Player::find_sbc_frame_len(&[0x9c, 0xBD, 0x35, 0xA2]).unwrap()
        );

        // 44.1, 16 blocks, Stereo, Loudness, 8 subbands, 53 bitpool (OS X)
        let header2 = [0x9c, 0xB9, 0x35, 0xA2];
        const HEADER2_FRAMELEN: usize = 118;
        let head = SbcHeader(u32::from_le_bytes(header2));
        assert!(head.has_syncword());
        assert_eq!(16, head.blocks());
        assert_eq!(ChannelMode::Stereo, head.channel_mode());
        assert_eq!(2, head.channels());
        assert_eq!(53, head.bitpool());
        assert_eq!(HEADER2_FRAMELEN, head.frame_length().unwrap());
        assert_eq!(HEADER2_FRAMELEN, Player::find_sbc_frame_len(&header2).unwrap());
    }

    pub(crate) fn expect_player_setup(
        exec: &mut fasync::Executor,
        audio_consumer_factory_request_stream: &mut SessionAudioConsumerFactoryRequestStream,
        codec_type: MediaCodecType,
        expected_session_id: u64,
    ) -> (StreamSinkRequestStream, AudioConsumerRequestStream, zx::Vmo) {
        let complete =
            exec.run_until_stalled(&mut audio_consumer_factory_request_stream.select_next_some());
        let audio_consumer_create_req = match complete {
            Poll::Ready(Ok(req)) => req,
            x => panic!("expected audio consumer create request message but got {:?}", x),
        };

        let (audio_consumer_create_request, session_id) = match audio_consumer_create_req {
            SessionAudioConsumerFactoryRequest::CreateAudioConsumer {
                audio_consumer_request,
                session_id,
                ..
            } => (audio_consumer_request, session_id),
        };

        assert_eq!(session_id, expected_session_id);

        let mut audio_consumer_request_stream =
            audio_consumer_create_request.into_stream().expect("audio consumer stream");

        let complete =
            exec.run_until_stalled(&mut audio_consumer_request_stream.select_next_some());
        let audio_consumer_req = match complete {
            Poll::Ready(Ok(req)) => req,
            x => panic!("expected audio consumer request message but got {:?}", x),
        };

        let (stream_sink_request, mut buffers, compression) = match audio_consumer_req {
            AudioConsumerRequest::CreateStreamSink {
                stream_sink_request,
                buffers,
                compression,
                ..
            } => (stream_sink_request, buffers, compression),
            _ => panic!("should be CreateStreamSink"),
        };

        match codec_type {
            MediaCodecType::AUDIO_SBC => assert_matches!(compression, None),
            MediaCodecType::AUDIO_AAC => assert_matches!(compression, Some(_)),
            _ => panic!("Unknown codec type"),
        };

        let sink_vmo = buffers.remove(0);

        sink_vmo.write(&[0], 0).expect_err("Write should fail");

        let sink_request_stream = stream_sink_request
            .into_stream()
            .expect("a sink request stream to be created from the request");

        (sink_request_stream, audio_consumer_request_stream, sink_vmo)
    }

    pub(crate) fn respond_event_status(
        exec: &mut fasync::Executor,
        audio_consumer_request_stream: &mut AudioConsumerRequestStream,
        status: AudioConsumerStatus,
    ) {
        let complete =
            exec.run_until_stalled(&mut audio_consumer_request_stream.select_next_some());
        let audio_consumer_req = match complete {
            Poll::Ready(Ok(req)) => req,
            x => panic!("expected audio consumer request message but got {:?}", x),
        };

        let watch_status_responder = match audio_consumer_req {
            AudioConsumerRequest::WatchStatus { responder, .. } => responder,
            _ => panic!("should be WatchStatus"),
        };

        watch_status_responder.send(status).expect("watch status sent");
    }

    /// Runs through the setup sequence of a AudioConsumer, returning the audio consumer,
    /// StreamSinkRequestStream and AudioConsumerRequestStream that it is communicating with, and
    /// the VMO payload buffer that was provided to the AudioConsumer.
    pub(crate) fn setup_player(
        mut exec: &mut fasync::Executor,
        codec_config: MediaCodecConfig,
    ) -> (Player, StreamSinkRequestStream, AudioConsumerRequestStream, zx::Vmo) {
        const TEST_SESSION_ID: u64 = 1;
        let codec_type = codec_config.codec_type().clone();

        let (audio_consumer_factory_proxy, mut audio_consumer_factory_request_stream) =
            create_proxy_and_stream::<SessionAudioConsumerFactoryMarker>()
                .expect("proxy pair creation");

        let mut player =
            Player::from_proxy(TEST_SESSION_ID, codec_config, audio_consumer_factory_proxy)
                .expect("player to build");

        let (sink_request_stream, mut audio_consumer_request_stream, sink_vmo) =
            expect_player_setup(
                &mut exec,
                &mut audio_consumer_factory_request_stream,
                codec_type,
                TEST_SESSION_ID,
            );

        {
            let player_next_event_fut = player.next_event();
            pin_mut!(player_next_event_fut);

            // player creation is done in stages, waiting for the below source/sink
            // objects to be created. Just run the creation up until the first
            // blocking point.
            assert!(exec.run_until_stalled(&mut player_next_event_fut).is_pending());

            respond_event_status(
                &mut exec,
                &mut audio_consumer_request_stream,
                AudioConsumerStatus {
                    min_lead_time: Some(50),
                    max_lead_time: Some(500),
                    error: None,
                    presentation_timeline: None,
                },
            );

            match exec.run_until_stalled(&mut player_next_event_fut) {
                Poll::Ready(PlayerEvent::Status(_s)) => {}
                x => panic!("Player should be ready with status but got {:?}", x),
            };
        }

        (player, sink_request_stream, audio_consumer_request_stream, sink_vmo)
    }

    fn build_config(codec_type: &MediaCodecType) -> MediaCodecConfig {
        match codec_type {
            &MediaCodecType::AUDIO_SBC => {
                MediaCodecConfig::build(codec_type.clone(), &[0x82, 0x15, 2, 250])
            }
            &MediaCodecType::AUDIO_AAC => MediaCodecConfig::build(codec_type.clone(), &[0; 6]),
            x => panic!("Can't build unknown codec type {:?}", x),
        }
        .expect("codec should build")
    }

    #[test]
    fn test_player_setup() {
        let mut exec = fasync::Executor::new().expect("executor should build");
        setup_player(&mut exec, build_config(&MediaCodecType::AUDIO_AAC));
    }

    #[test]
    fn test_player_closed() {
        let mut exec = fasync::Executor::new().expect("executor should build");
        let (mut player, _sink_request_stream, mut audio_consumer_request_stream, _sink_vmo) =
            setup_player(&mut exec, build_config(&MediaCodecType::AUDIO_AAC));
        let player_next_event_fut = player.next_event();
        pin_mut!(player_next_event_fut);

        // player creation is done in stages, waiting for the below source/sink
        // objects to be created. Just run the creation up until the first
        // blocking point.
        assert!(exec.run_until_stalled(&mut player_next_event_fut).is_pending());

        let complete =
            exec.run_until_stalled(&mut audio_consumer_request_stream.select_next_some());
        let watch_status_responder = match complete {
            Poll::Ready(Ok(AudioConsumerRequest::WatchStatus { responder, .. })) => responder,
            x => panic!("expected audio consumer WatchStatus request but got {:?}", x),
        };

        drop(watch_status_responder);
        drop(audio_consumer_request_stream);

        match exec.run_until_stalled(&mut player_next_event_fut) {
            Poll::Ready(PlayerEvent::Closed) => {}
            x => panic!("Expected player to be closed, got {:?}", x),
        };
    }

    const AUDIO_MUX_LENGTH: usize = 928;
    const AAC_RTP_PACKET_LENGTH: usize = RtpHeader::LENGTH + AUDIO_MUX_LENGTH;
    const AAC_HEADER_LENGTH: usize = 13;

    fn build_rtp_aac_packet(payload: &[u8]) -> [u8; AAC_RTP_PACKET_LENGTH] {
        // raw rtp header with sequence number of 1 followed by 1 aac AudioMuxElement with 0's for
        // payload
        let rtp: &[u8] = &[128, 96, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0];
        let aac_header: &[u8] = &[71, 252, 0, 0, 176, 144, 128, 3, 0, 255, 255, 255, 150];
        let mut raw: [u8; AAC_RTP_PACKET_LENGTH] = [0; AAC_RTP_PACKET_LENGTH];

        raw[0..RtpHeader::LENGTH].copy_from_slice(rtp);
        const MUX_ELEMENT_START: usize = RtpHeader::LENGTH + AAC_HEADER_LENGTH;
        raw[RtpHeader::LENGTH..MUX_ELEMENT_START].copy_from_slice(aac_header);
        raw[MUX_ELEMENT_START..(MUX_ELEMENT_START + payload.len())].copy_from_slice(payload);
        raw
    }

    #[test]
    /// Tests that the creation of a player executes with the expected interaction with the
    /// AudioConsumer and stream setup.
    /// This tests that the buffer is sent correctly and that data "sent" through the shared
    /// VMO is readable by the receiver of the VMO.
    /// We do this by mocking the AudioConsumer and StreamSink interfaces that are used.
    fn test_send_frame() {
        let mut exec = fasync::Executor::new().expect("executor should build");

        let (mut player, mut sink_request_stream, _player_request_stream, sink_vmo) =
            setup_player(&mut exec, build_config(&MediaCodecType::AUDIO_AAC));

        let content = &[1, 2, 3, 4, 5, 6, 7, 8, 9];
        let payload = build_rtp_aac_packet(content);

        let payload_fut = player.push_payload(&payload);
        pin_mut!(payload_fut);

        match exec.run_singlethreaded(&mut payload_fut) {
            Ok(()) => {}
            x => panic!("Expected push_payload Ok(()) but got {:?}", x),
        }

        let sink_req = exec
            .run_singlethreaded(&mut sink_request_stream.select_next_some())
            .expect("got a packet");
        //let sink_req = match complete {
        //    Poll::Ready(Ok(req)) => req,
        //    x => panic!("expected sink request message but got {:?}", x),
        //};

        let (offset, size) = match sink_req {
            StreamSinkRequest::SendPacketNoReply { packet, .. } => {
                (packet.payload_offset, packet.payload_size as usize)
            }
            _ => panic!("should have received a packet"),
        };

        let mut recv = Vec::with_capacity(size);
        recv.resize(size, 0);

        sink_vmo.read(recv.as_mut_slice(), offset).expect("should be able to read packet data");

        assert_eq!(&recv[..content.len()], content, "received didn't match payload");
    }

    /// Helper function for pushing payloads to player and returning the packet flags
    fn push_payload_get_flags(
        payload: &[u8],
        exec: &mut fasync::Executor,
        player: &mut Player,
        sink_request_stream: &mut StreamSinkRequestStream,
    ) -> u32 {
        {
            let push_fut = player.push_payload(payload);
            pin_mut!(push_fut);
            exec.run_singlethreaded(&mut push_fut).expect("wrote payload");
        }
        // Drive until the sink receives a packet.
        let sink_req = exec
            .run_singlethreaded(&mut sink_request_stream.select_next_some())
            .expect("sent packet");
        match sink_req {
            StreamSinkRequest::SendPacketNoReply { packet, .. } => packet.flags,
            _ => panic!("should have received a packet"),
        }
    }

    #[test]
    /// Test that discontinuous packets are flagged as such. We do this by
    /// sending packets through a Player and examining them after they come out
    /// of the mock StreamSink interface.
    fn test_packet_discontinuities() {
        let mut exec = fasync::Executor::new().expect("executor should build");

        let (mut player, mut sink_request_stream, mut player_request_stream, _) =
            setup_player(&mut exec, build_config(&MediaCodecType::AUDIO_AAC));

        let mut raw = build_rtp_aac_packet(&[]);

        let flags = push_payload_get_flags(&raw, &mut exec, &mut player, &mut sink_request_stream);
        // should not have a discontinuity yet
        assert_eq!(flags & STREAM_PACKET_FLAG_DISCONTINUITY, 0);

        // Should have started the player when the first packet gets pushed.
        let complete = exec.run_until_stalled(&mut player_request_stream.select_next_some());
        let player_req = match complete {
            Poll::Ready(Ok(req)) => req,
            x => panic!("expected player req message but got {:?}", x),
        };

        assert_matches!(player_req, AudioConsumerRequest::Start { .. });

        // increment sequence number
        raw[3] = 2;
        let flags = push_payload_get_flags(&raw, &mut exec, &mut player, &mut sink_request_stream);
        // should not have a discontinuity yet
        assert_eq!(flags & STREAM_PACKET_FLAG_DISCONTINUITY, 0);

        // introduce discont
        raw[3] = 8;
        let flags = push_payload_get_flags(&raw, &mut exec, &mut player, &mut sink_request_stream);
        assert_eq!(flags & STREAM_PACKET_FLAG_DISCONTINUITY, STREAM_PACKET_FLAG_DISCONTINUITY);
    }

    #[test]
    /// Test that parsing works when pushing an AAC packet
    fn test_aac_parsing() {
        let mut exec = fasync::Executor::new().expect("executor should build");

        let (mut player, mut sink_request_stream, mut player_request_stream, _) =
            setup_player(&mut exec, build_config(&MediaCodecType::AUDIO_AAC));

        let mut raw = build_rtp_aac_packet(&[]);
        push_payload_get_flags(&raw, &mut exec, &mut player, &mut sink_request_stream);

        // Should have started the player
        let complete = exec.run_until_stalled(&mut player_request_stream.select_next_some());
        let player_req = match complete {
            Poll::Ready(Ok(req)) => req,
            x => panic!("expected player req message but got {:?}", x),
        };

        assert_matches!(player_req, AudioConsumerRequest::Start { .. });

        // corrupt AudioMuxElement
        raw[RtpHeader::LENGTH + 1] = 0xff;

        let push_fut = player.push_payload(&raw);
        pin_mut!(push_fut);
        exec.run_singlethreaded(&mut push_fut).expect_err("fail to write corrupted payload");
    }

    #[test]
    /// Test that bytes flow through to decoder when SBC is active
    fn test_sbc_decoder_write() {
        let mut exec = fasync::Executor::new().expect("executor should build");

        let (mut player, mut sink_request_stream, mut player_request_stream, _) =
            setup_player(&mut exec, build_config(&MediaCodecType::AUDIO_SBC));

        // raw rtp header with sequence number of 1 followed by 1 sbc frame
        let raw = [
            128, 96, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x9c, 0xb1, 0x20, 0x3b, 0x80, 0x00, 0x00,
            0x11, 0x7f, 0xfa, 0xab, 0xef, 0x7f, 0xfa, 0xab, 0xef, 0x80, 0x4a, 0xab, 0xaf, 0x80,
            0xf2, 0xab, 0xcf, 0x83, 0x8a, 0xac, 0x32, 0x8a, 0x78, 0x8a, 0x53, 0x90, 0xdc, 0xad,
            0x49, 0x96, 0xba, 0xaa, 0xe6, 0x9c, 0xa2, 0xab, 0xac, 0xa2, 0x72, 0xa9, 0x2d, 0xa8,
            0x9a, 0xab, 0x75, 0xae, 0x82, 0xad, 0x49, 0xb4, 0x6a, 0xad, 0xb1, 0xba, 0x52, 0xa9,
            0xa8, 0xc0, 0x32, 0xad, 0x11, 0xc6, 0x5a, 0xab, 0x3a,
        ];

        push_payload_get_flags(&raw, &mut exec, &mut player, &mut sink_request_stream);

        // Should have started the player
        let complete = exec.run_until_stalled(&mut player_request_stream.select_next_some());
        let player_req = match complete {
            Poll::Ready(Ok(req)) => req,
            x => panic!("expected player req message but got {:?}", x),
        };

        assert_matches!(player_req, AudioConsumerRequest::Start { .. });
    }
}
