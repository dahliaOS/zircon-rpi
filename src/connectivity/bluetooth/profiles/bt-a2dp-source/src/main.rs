// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]
#![recursion_limit = "1024"]

use {
    byteorder::{ByteOrder, NativeEndian},
    bt_avdtp as avdtp,
    fuchsia_media::{self, audio::{AudioFrameStream,Encoder,SoftPcmAudioOutput}},
    failure::{format_err, Error, ResultExt},
    fidl_fuchsia_bluetooth_bredr::*,
    fidl_fuchsia_media::*,
    fidl_fuchsia_mediacodec::*,
    fuchsia_async::{self as fasync, Timer},
    parking_lot::RwLock,
    fuchsia_syslog::{self, fx_log_info, fx_log_warn},
    fuchsia_zircon::{self as zx, Duration, Time},
    futures::{channel::mpsc::{self, Sender, Receiver}, task::{Poll, Context}, Future, FutureExt, SinkExt, stream, StreamExt, select, self},
    std::{collections::{HashMap, hash_map::Entry}, string::String, pin::Pin, sync::Arc, num::Wrapping},
};

/// Make the SDP definition for the A2DP source service.
fn make_profile_service_definition() -> ServiceDefinition {
    ServiceDefinition {
        service_class_uuids: vec![String::from("110A")], // Audio Source UUID
        protocol_descriptors: vec![
            ProtocolDescriptor {
                protocol: ProtocolIdentifier::L2Cap,
                params: vec![DataElement {
                    type_: DataElementType::UnsignedInteger,
                    size: 2,
                    data: DataElementData::Integer(PSM_AVDTP),
                }],
            },
            ProtocolDescriptor {
                protocol: ProtocolIdentifier::Avdtp,
                params: vec![DataElement {
                    type_: DataElementType::UnsignedInteger,
                    size: 2,
                    data: DataElementData::Integer(0x0103), // Indicate v1.3
                }],
            },
        ],
        profile_descriptors: vec![ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 3,
        }],
        additional_protocol_descriptors: None,
        information: vec![Information {
            language: "en".to_string(),
            name: Some("A2DP".to_string()),
            description: Some("Advanced Audio Distribution Profile".to_string()),
            provider: Some("Fuchsia".to_string()),
        }],
        additional_attributes: None,
    }
}

// SDP Attribute ID for the Supported Features of A2DP.
// Defined in Assigned Numbers for SDP
// https://www.bluetooth.com/specifications/assigned-numbers/service-discovery
const ATTR_A2DP_SUPPORTED_FEATURES: u16 = 0x0311;

// Defined in the Bluetooth Assigned Numbers for Audio/Video applications
// https://www.bluetooth.com/specifications/assigned-numbers/audio-video
const AUDIO_CODEC_SBC: u8 = 0;
// Arbitrarily chosen ID for the SBC stream endpoint.
const SBC_SEID: u8 = 6;

/// Controls a stream endpoint and the media decoding task which is associated with it.
struct Stream {
    /// The AVDTP endpoint that this stream is associated with.
    endpoint: avdtp::StreamEndpoint,
    /// The encoding that media sent to this endpoint should be encoded with.
    /// This should be an encoding constant from fuchsia.media like AUDIO_ENCODING_SBC.
    /// See //sdk/fidl/fuchsia.media/stream_type.fidl for valid encodings.
    encoding: String,
}

impl Stream {
    fn new(endpoint: avdtp::StreamEndpoint, encoding: String) -> Stream {
        Stream { endpoint, encoding }
    }

    /// Attempt to start the media decoding task.
    fn start(&mut self) -> Result<(), avdtp::ErrorCode> {
        fx_log_info!("Attempt to start with {} encoding", self.encoding);
        return Err(avdtp::ErrorCode::NotSupportedCommand);
    }

    /// Signals to the media decoding task to end.
    fn stop(&mut self) -> Result<(), avdtp::ErrorCode> {
        fx_log_info!("Attempt to stop with {} encoding", self.encoding);
        return Err(avdtp::ErrorCode::NotSupportedCommand);
    }
}

struct Streams(HashMap<avdtp::StreamEndpointId, Stream>);

impl Streams {
    /// A new empty set of endpoints.
    fn new() -> Streams {
        Streams(HashMap::new())
    }

    /// Builds a set of endpoints from the available codecs.
    fn build() -> avdtp::Result<Streams> {
        let mut s = Streams::new();
        // TODO(BT-533): detect codecs, add streams for each codec
        let sbc_stream = avdtp::StreamEndpoint::new(
            SBC_SEID,
            avdtp::MediaType::Audio,
            avdtp::EndpointType::Source,
            vec![
                avdtp::ServiceCapability::MediaTransport,
                avdtp::ServiceCapability::MediaCodec {
                    media_type: avdtp::MediaType::Audio,
                    codec_type: avdtp::MediaCodecType::new(AUDIO_CODEC_SBC),
                    // SBC Codec Specific Information Elements:
                    // These are the mandatory support in source.
                    // Byte 0:
                    //  - Sampling Frequencies: 44.1kHz
                    //  - Channel modes: (MONO, JOINT STEREO)
                    // Byte 1:
                    //  - Block length: all (4, 8, 12, 16)
                    //  - Subbands: 8
                    //  - Allocation Method: Loudness
                    // Byte 2-3: Minimum and maximum bitpool value. This is just the minimum to the max.
                    // TODO(jamuraa): there should be a way to build this data in a structured way (bt-a2dp?)
                    codec_extra: vec![0x29, 0xF5, 2, 250],
                },
            ],
        )?;
        s.insert(sbc_stream, AUDIO_ENCODING_SBC.to_string());
        Ok(s)
    }

    /// Adds a stream, indexing it by the endoint id, associated with an encoding,
    /// replacing any other stream with the same endpoint id.
    fn insert(&mut self, stream: avdtp::StreamEndpoint, codec: String) {
        self.0.insert(stream.local_id().clone(), Stream::new(stream, codec));
    }

    /// Retrievees a mutable reference to the endpoint with the `id`.
    fn get_endpoint(&mut self, id: &avdtp::StreamEndpointId) -> Option<&mut avdtp::StreamEndpoint> {
        self.0.get_mut(id).map(|x| &mut x.endpoint)
    }

    /// Retrieves a mutable reference to the Stream referenced by `id`, if the stream exists,
    /// otherwise returns Err(BadAcpSeid).
    fn get_mut(&mut self, id: &avdtp::StreamEndpointId) -> Result<&mut Stream, avdtp::ErrorCode> {
        self.0.get_mut(id).ok_or(avdtp::ErrorCode::BadAcpSeid)
    }

    /// Returns the information on all known streams.
    fn information(&self) -> Vec<avdtp::StreamInformation> {
        self.0.values().map(|x| x.endpoint.information()).collect()
    }

    /// Finds a compatible stream in this collection for the StreamEndpoint.
    fn find_match(&mut self, remote: &avdtp::StreamEndpoint) -> Option<&mut avdtp::StreamEndpoint> {
        self.0.values_mut().find(|v| v.endpoint.compatible_with(remote)).map(|x| &mut x.endpoint)
    }
}

/// RemotePeer handles requests from the AVDTP layer, and provides responses as appropriate based
/// on the current state of the A2DP streams available.
/// Each remote peer has its own set of stream endpoints.
struct RemotePeer {
    /// AVDTP peer communicating to this.
    peer: Arc<avdtp::Peer>,
    /// Some(id) if we are opening a StreamEndpoint but haven't finished yet.
    /// AVDTP Sec 6.11 - only up to one stream can be in this state.
    opening: Option<avdtp::StreamEndpointId>,
    /// The stream endpoint collection for this peer.
    streams: Streams,
}

type RemotesMap = HashMap<String, RemotePeer>;

impl RemotePeer {
    fn new(peer: avdtp::Peer) -> RemotePeer {
        RemotePeer { peer: Arc::new(peer), opening: None, streams: Streams::build().unwrap() }
    }

    /// Provides a reference to the AVDTP peer.
    fn peer(&self) -> Arc<avdtp::Peer> {
        self.peer.clone()
    }

    /// Provide a new established L2CAP channel to this remote peer.
    /// This function should be called whenever the remote assoiated with this peer opens an
    /// L2CAP channel after the first.
    fn receive_channel(&mut self, channel: zx::Socket) -> Result<(), Error> {
        let stream = match &self.opening {
            None => Err(format_err!("No stream opening.")),
            Some(id) => self.streams.get_endpoint(&id).ok_or(format_err!("endpoint doesn't exist")),
        }?;
        if !stream.receive_channel(fasync::Socket::from_socket(channel)?)? {
            self.opening = None;
        }
        fx_log_info!("connected transport channel to seid {}", stream.local_id());
        Ok(())
    }

    /// Start an asynchronous task to handle any requests from the AVDTP peer.
    /// This task completes when the remote end closes the signaling connection.
    /// This remote peer should be active in the `remotes` map with an id of `device_id`.
    /// When the signaling connection is closed, the task deactivates the remote, removing it
    /// from the `remotes` map.
    fn start_requests_task(&mut self, remotes: Arc<RwLock<RemotesMap>>, device_id: String) {
        let mut request_stream = self.peer.take_request_stream();
        fuchsia_async::spawn(
            async move {
                while let Some(r) = await!(request_stream.next()) {
                    match r {
                        Err(e) => fx_log_info!("Request Error on {}: {:?}", device_id, e),
                        Ok(request) => {
                            let mut peer;
                            {
                                let mut wremotes = remotes.write();
                                peer = wremotes.remove(&device_id).unwrap();
                            }
                            let fut = peer.handle_request(request);
                            if let Err(e) = await!(fut) {
                                fx_log_warn!("{} Error handling request: {:?}", device_id, e);
                            }
                            remotes.write().insert(device_id.clone(), peer);
                        }
                    }
                }
                fx_log_info!("Peer {} disconnected", device_id);
                remotes.write().remove(&device_id);
            },
        );
    }

    fn streams(&mut self) -> &mut Streams {
        &mut self.streams
    }

    /// Handle a single request event from the avdtp peer.
    async fn handle_request(&mut self, r: avdtp::Request) -> avdtp::Result<()> {
        fx_log_info!("Handling {:?} from peer..", r);
        match r {
            avdtp::Request::Discover { responder } => responder.send(&self.streams.information()),
            avdtp::Request::GetCapabilities { responder, stream_id }
            | avdtp::Request::GetAllCapabilities { responder, stream_id } => {
                match self.streams.get_endpoint(&stream_id) {
                    None => responder.reject(avdtp::ErrorCode::BadAcpSeid),
                    Some(stream) => responder.send(stream.capabilities()),
                }
            }
            avdtp::Request::Open { responder, stream_id } => {
                match self.streams.get_endpoint(&stream_id) {
                    None => responder.reject(avdtp::ErrorCode::BadAcpSeid),
                    Some(stream) => match stream.establish() {
                        Ok(()) => {
                            self.opening = Some(stream_id);
                            responder.send()
                        }
                        Err(_) => responder.reject(avdtp::ErrorCode::BadState),
                    },
                }
            }
            avdtp::Request::Close { responder, stream_id } => {
                match self.streams.get_endpoint(&stream_id) {
                    None => responder.reject(avdtp::ErrorCode::BadAcpSeid),
                    Some(stream) => await!(stream.release(responder, &self.peer)),
                }
            }
            avdtp::Request::SetConfiguration {
                responder,
                local_stream_id,
                remote_stream_id,
                capabilities,
            } => {
                let stream = match self.streams.get_endpoint(&local_stream_id) {
                    None => return responder.reject(None, avdtp::ErrorCode::BadAcpSeid),
                    Some(stream) => stream,
                };
                // TODO(BT-695): Confirm the MediaCodec parameters are OK
                match stream.configure(&remote_stream_id, capabilities) {
                    Ok(_) => responder.send(),
                    Err(e) => {
                        // Only happens when this is already configured.
                        responder.reject(None, avdtp::ErrorCode::SepInUse)?;
                        Err(e)
                    }
                }
            }
            avdtp::Request::GetConfiguration { stream_id, responder } => {
                let stream = match self.streams.get_endpoint(&stream_id) {
                    None => return responder.reject(avdtp::ErrorCode::BadAcpSeid),
                    Some(stream) => stream,
                };
                match stream.get_configuration() {
                    Ok(c) => responder.send(&c),
                    Err(e) => {
                        // Only happens when the stream is in the wrong state
                        responder.reject(avdtp::ErrorCode::BadState)?;
                        Err(e)
                    }
                }
            }
            avdtp::Request::Reconfigure { responder, local_stream_id, capabilities } => {
                let stream = match self.streams.get_endpoint(&local_stream_id) {
                    None => return responder.reject(None, avdtp::ErrorCode::BadAcpSeid),
                    Some(stream) => stream,
                };
                // TODO(jamuraa): Actually tweak the codec parameters.
                match stream.reconfigure(capabilities) {
                    Ok(_) => responder.send(),
                    Err(e) => {
                        responder.reject(None, avdtp::ErrorCode::BadState)?;
                        Err(e)
                    }
                }
            }
            avdtp::Request::Start { responder, stream_ids } => {
                for seid in stream_ids {
                    if let Err(code) = self.streams.get_mut(&seid).and_then(|x| x.start()) {
                        return responder.reject(&seid, code);
                    }
                }
                responder.send()
            }
            avdtp::Request::Suspend { responder, stream_ids } => {
                for seid in stream_ids {
                    if let Err(code) = self.streams.get_mut(&seid).and_then(|x| x.stop()) {
                        return responder.reject(&seid, code);
                    }
                }
                responder.send()
            }
            avdtp::Request::Abort { responder, stream_id } => {
                let stream = match self.streams.get_endpoint(&stream_id) {
                    None => return Ok(()),
                    Some(stream) => stream,
                };
                await!(stream.abort(None))?;
                self.opening = self.opening.take().filter(|id| id != &stream_id);
                let _ = self.streams.get_mut(&stream_id).and_then(|x| x.stop());
                responder.send()
            }
        }
    }
}

fn transform_u32_to_array_of_u8(x:u32) -> [u8;4] {
    let b1 : u8 = ((x >> 24) & 0xff) as u8;
    let b2 : u8 = ((x >> 16) & 0xff) as u8;
    let b3 : u8 = ((x >> 8) & 0xff) as u8;
    let b4 : u8 = (x & 0xff) as u8;
    return [b1, b2, b3, b4]
}

async fn send_packets_task(mut receiver: Receiver<Vec<u8>>, transport: zx::Socket) {
    let mut next_packet_time = Time::INFINITE_PAST;
    loop {
        let packet = await!(receiver.select_next_some());
        let now = zx::Time::get(zx::ClockId::Monotonic);
        if now > next_packet_time {
            if next_packet_time == Time::INFINITE_PAST {
                next_packet_time = now;
                fx_log_info!("First packet, sending now");
            } else {
                fx_log_info!("Packet {} ms overdue, sending now", (now - next_packet_time).into_millis());
            }
        } else {
            await!(Timer::new(next_packet_time));
        }
        if let Err(e) = transport.write(&packet) {
            fx_log_info!("Failed sending media packet to peer: {}", e);
            return;
        }
        next_packet_time += Duration::from_micros(14500);
    }
}

#[allow(dead_code)]
fn make_device_audio_out_stream() -> Result<AudioFrameStream, Error> {
    let pcm_format = PcmFormat {
        pcm_mode: AudioPcmMode::Linear,
        bits_per_sample: 16,
        frames_per_second: 44100,
        channel_map: vec![AudioChannelId::Lf, AudioChannelId::Rf]
    };

    let (chan, frame_stream) = SoftPcmAudioOutput::build(&[1; 16], "Google", "rust-audio-test", pcm_format, zx::Duration::from_millis(20))?;

    let svc = fuchsia_component::client::connect_to_service::<AudioDeviceEnumeratorMarker>()
        .context("Failed to connect to AudioDeviceEnumerator")?;

    fx_log_info!("connected to the audio core");
    let _ = svc.add_device_by_channel(chan, "rust-audio-stream", false)?;

    Ok(frame_stream)
}

struct SawWaveStream {
    format: PcmFormat,
    frequency_hops: Vec<f32>,
    next_frame_timer: fasync::Timer,
    /// the last time we delivered frames.
    last_frame_time: Option<zx::Time>,
}

const PCM_SAMPLE_SIZE: usize = 2;

fn create_saw_wave(frequency: f32, pcm_format: &PcmFormat, frame_count: usize) -> Vec<u8> {
    const AMPLITUDE: f32 = 0.2;

    let pcm_frame_size = PCM_SAMPLE_SIZE * pcm_format.channel_map.len();
    let samples_per_frame = pcm_format.channel_map.len();
    let sample_count = frame_count * samples_per_frame;

    let mut buffer = vec![0; frame_count * pcm_frame_size];

    for i in 0..sample_count {
        let frame = (i / samples_per_frame) as f32;
        let value =
            ((frame * frequency / (pcm_format.frames_per_second as f32)) % 1.0) * AMPLITUDE;
        let sample = (value * i16::max_value() as f32) as i16;

        let mut sample_bytes = [0; std::mem::size_of::<i16>()];
        NativeEndian::write_i16(&mut sample_bytes, sample);

        let offset = i * PCM_SAMPLE_SIZE;
        buffer[offset] = sample_bytes[0];
        buffer[offset + 1] = sample_bytes[1];
    }
    buffer
}

impl futures::Stream for SawWaveStream {
    type Item = Result<Vec<u8>, fuchsia_media::Error>;


    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let now = zx::Time::get(zx::ClockId::Monotonic);
        if self.last_frame_time.is_none() {
            self.last_frame_time = Some(now - Duration::from_seconds(1));
        }
        let last_time = self.last_frame_time.as_ref().unwrap().clone();
        let repeats = (now - last_time).into_seconds();
        if repeats == 0 {
            self.next_frame_timer = fasync::Timer::new(last_time + Duration::from_seconds(1));
            let poll = self.next_frame_timer.poll_unpin(cx);
            assert!(poll == Poll::Pending);
            return Poll::Pending;
        }
        let next_freq = self.frequency_hops.remove(0);
        let frames = create_saw_wave(next_freq, &self.format, self.format.frames_per_second as usize);
        self.frequency_hops.push(next_freq);
        self.last_frame_time = Some(last_time + Duration::from_seconds(1));
        Poll::Ready(Some(Ok(frames)))
    }
}

impl stream::FusedStream for SawWaveStream {
    fn is_terminated(&self) -> bool {
        false
    }
}

#[allow(dead_code)]
fn make_saw_wave_stream() -> SawWaveStream {
    let format = PcmFormat {
        pcm_mode: AudioPcmMode::Linear,
        bits_per_sample: 16,
        frames_per_second: 44100,
        channel_map: vec![AudioChannelId::Lf, AudioChannelId::Rf]
    };
    SawWaveStream {
        format,
        // G# - 415.30 F# - 369.99 E - 329.63 B - 246.94
        // Chimes: (silence) E, G#, F#, B, E, F#, G#, E, G#, E, F#, B, B, F#, G# E
        frequency_hops: vec![0.0, 329.63, 415.30, 369.99, 246.94,
                         329.63, 369.99, 415.30, 329.63,
                         415.30, 329.63, 369.99, 246.94,
                         246.94, 369.99, 415.30, 329.63],
        next_frame_timer: fasync::Timer::new(zx::Time::INFINITE_PAST),
        last_frame_time: None,
    }
}

/// packetizes and encodes SBC audio frames, and sends them to `sender`
#[allow(dead_code)]
async fn sbc_encode_stream(mut frame_stream: impl futures::Stream<Item = Result<Vec<u8>, fuchsia_media::Error>> + stream::FusedStream + Unpin, mut sender: Sender<Vec<u8>>) -> Result<(), Error> {
    let sbc_encoder_settings = EncoderSettings::Sbc(SbcEncoderSettings {
        sub_bands: SbcSubBands::SubBands8,
        allocation: SbcAllocation::AllocLoudness,
        block_count: SbcBlockCount::BlockCount16,
        channel_mode: SbcChannelMode::JointStereo,
        bit_pool: 53,
    });

    let pcm_format = PcmFormat {
        pcm_mode: AudioPcmMode::Linear,
        bits_per_sample: 16,
        frames_per_second: 44100,
        channel_map: vec![AudioChannelId::Lf, AudioChannelId::Rf]
    };

    let sbc_format_details = FormatDetails {
        domain: Some(DomainFormat::Audio(AudioFormat::Uncompressed(AudioUncompressedFormat::Pcm(pcm_format)))),
        encoder_settings: Some(sbc_encoder_settings),
        format_details_version_ordinal: Some(1),
        mime_type: Some("audio/pcm".to_string()),
        oob_bytes: None,
        pass_through_parameters: None,
        timebase: None,
    };

    let encoder_params = CreateEncoderParams {
        input_details: Some(sbc_format_details),
        require_hw: Some(false),
    };

    let mut encoder = Encoder::start(encoder_params)?;

    let mut sequence_number = Wrapping(1u16); // Advances by 1 every payload
    let one = Wrapping(1u16);
    let mut timestamp: u32 = 0; // Advances by 640 every paylaod
    let mut frames: u64 = 0;
    let mut packet = Vec::with_capacity(13 + 119 * 5);

    let mut encoder_output = encoder.take_encoded_stream();

    loop {
        let mut frames_fut = frame_stream.select_next_some();
        let mut encoded_fut = encoder_output.select_next_some();
        select! {
            encoded = encoded_fut => {
                let mut payload = encoded?;
                if !(payload[0] == 0x9c) {
                    fx_log_info!("SBC syncword not found, ignoring frame");
                    continue;
                }
                if (frames % 5) == 0 {
                    if !(frames == 0) {
                        await!(sender.send(packet))?;
                    }
                    packet = Vec::with_capacity(13 + 119 * 5);
                    packet.extend_from_slice(&[0x80, 0x60]);
                    packet.push((sequence_number.0 >> 8) as u8);
                    packet.push((sequence_number.0 & 0xFF) as u8);
                    packet.extend_from_slice(&transform_u32_to_array_of_u8(timestamp));
                    packet.extend_from_slice(&[0, 0, 0, 0]);
                    // SBC Payload Header - not fragmented, num of frames
                    packet.extend_from_slice(&[5]);
                    sequence_number += one;
                    timestamp += 640;
                }
                packet.extend_from_slice(&payload[..]);
                frames += 1;
            }
            frame_data = frames_fut => {
                let data = frame_data?;
                encoder.deliver_input(&data)?;
            },
            complete => return Ok(()),
        }
    }
}

async fn log_result<F>(name: String, f: F)
    where F: Future<Output = avdtp::Result<()>>
{
    match await!(f) {
        Err(e) => fx_log_warn!("{} ended with error: {:?}", name, e),
        Ok(()) => fx_log_info!("{} ended", name),
    };
}

async fn log_stdresult<F>(name: String, f: F)
    where F: Future<Output = Result<(), Error>>
{
    match await!(f) {
        Err(e) => fx_log_warn!("{} ended with Error: {:?}", name, e),
        Ok(()) => fx_log_info!("{} ended", name),
    };
}

/// Probe the remote peer for a candidate sink endpoint, configure it, and start streaming.
// TODO(jamuraa): this needs to take the version number into account because get_all_capabilities
// is not available in older versions?
async fn discover_and_start(peer: Arc<avdtp::Peer>, profile_svc: ProfileProxy, peer_id: String, remotes: Arc<RwLock<RemotesMap>>) -> avdtp::Result<()> {
    fx_log_info!("Looking for a candidate stream..");
    let infos = match await!(peer.discover()) {
        Ok(infos) => infos,
        Err(e) => {
            fx_log_info!("Peer {}: failed to discover source streams: {}", peer_id, e);
            remotes.write().remove(&peer_id);
            return Ok(());
        }
    };
    fx_log_info!("Discovered {} streams", infos.len());
    let mut remote_streams = Vec::new();
    for info in infos {
        match await!(peer.get_capabilities(info.id())) {
            Ok(capabilities) => {
                fx_log_info!("Stream {:?}", info);
                for cap in &capabilities {
                    fx_log_info!("  - {:?}", cap);
                }
                remote_streams.push(avdtp::StreamEndpoint::from_info(&info, capabilities));
            }
            Err(e) => {
                fx_log_info!("Stream {} capabilities failed: {:?}", info.id(), e);
                return Err(avdtp::Error::OutOfRange);
            }
        };
    }
    let stream;
    let local_stream_id;
    match remotes.write().get_mut(&peer_id) {
            None => return Err(avdtp::Error::PeerDisconnected),
            Some(wpeer) => {
                stream = match remote_streams.iter().find(|x| wpeer.streams().find_match(x).is_some() ) {
                    Some(s) => s,
                    None => {
                        fx_log_info!("No candidate stream found.  Disconnecting.");
                        // TODO: actually disconnect here
                        return Ok(());
                    }
                };
                local_stream_id = wpeer.streams().find_match(stream).unwrap().local_id().clone();
            }
    }
    let sbc_capabilities = vec![
        avdtp::ServiceCapability::MediaTransport,
        avdtp::ServiceCapability::MediaCodec {
            media_type: avdtp::MediaType::Audio,
            codec_type: avdtp::MediaCodecType::new(AUDIO_CODEC_SBC),
            codec_extra: vec![0x21, 0x15, 2, 53],
        }];
    await!(peer.set_configuration(stream.local_id(), &local_stream_id, &sbc_capabilities))?;
    await!(peer.open(stream.local_id()))?;
    // TODO: have peer.open() actually open the l2cap connections
    let (status, channel) = await!(profile_svc.connect_l2cap(&peer_id, PSM_AVDTP as u16)).unwrap();
    if let Some(e) = status.error {
        fx_log_warn!("Couldn't connect media transport {}: {:?}", peer_id, e);
        return Err(avdtp::Error::PeerDisconnected);
    }
    if channel.is_none() {
        fx_log_warn!("Couldn't connect media transport {}: no channel", peer_id);
        return Err(avdtp::Error::PeerDisconnected);
    }
    let peers_to_start = [stream.local_id().clone()];
    await!(peer.start(&peers_to_start))?;

    let (encoded_sender, encoded_receiver) = mpsc::channel(5);
    fuchsia_async::spawn(send_packets_task(encoded_receiver, channel.unwrap()));
    let audio_out_stream = make_device_audio_out_stream().map_err(|_e| avdtp::Error::Encoding)?;
    //let audio_out_stream = make_saw_wave_stream();
    fuchsia_async::spawn(log_stdresult("sbc_encode_stream".to_string(), sbc_encode_stream(audio_out_stream, encoded_sender)));
    Ok(())
}



#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["a2dp-source"]).expect("Can't init logger");

    let profile_svc = fuchsia_component::client::connect_to_service::<ProfileMarker>()
        .context("Failed to connect to Bluetooth profile service")?;

    let mut service_def = make_profile_service_definition();
    let (status, service_id) = await!(profile_svc.add_service(
        &mut service_def,
        SecurityLevel::EncryptionOptional,
        false
    ))?;

    if let Some(e) = status.error {
        return Err(format_err!("Couldn't add A2DP source service: {:?}", e));
    }

    fx_log_info!("Registered Service ID {}", service_id);

    let attrs: Vec<u16> = vec![
        ATTR_PROTOCOL_DESCRIPTOR_LIST,
        ATTR_SERVICE_CLASS_ID_LIST,
        ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
        ATTR_A2DP_SUPPORTED_FEATURES,
    ];

    profile_svc.add_search(ServiceClassProfileIdentifier::AudioSink, &mut attrs.into_iter())?;

    let remotes: Arc<RwLock<RemotesMap>> = Arc::new(RwLock::new(HashMap::new()));

    let mut evt_stream = profile_svc.take_event_stream();
    while let Some(evt) = await!(evt_stream.next()) {
        match evt? {
            ProfileEvent::OnServiceFound { peer_id, profile, attributes } => {
                fx_log_info!("Audio Sink on {} with profile {:?}: {:?}",
                             peer_id, profile, attributes);
                match remotes.write().entry(peer_id.clone()) {
                    Entry::Occupied(_) => {
                        fx_log_info!("Not connecting to already-connected peer");
                        continue;
                    }
                    Entry::Vacant(entry) => {
                        fx_log_info!("Connecting to new sink {}..", peer_id);
                        let (status, channel) = await!(profile_svc.connect_l2cap(&peer_id, PSM_AVDTP as u16))?;

                        if let Some(e) = status.error {
                            fx_log_warn!("Couldn't connect to {}: {:?}", peer_id, e);
                            continue;
                        }
                        if channel.is_none() {
                            fx_log_warn!("Couldn't connect {}: no channel", peer_id);
                            continue;
                        }
                        let peer = match avdtp::Peer::new(channel.unwrap()) {
                            Ok(peer) => peer,
                            Err(e) => {
                                fx_log_warn!("Error adding new peer {}: {:?}", peer_id, e);
                                continue;
                            }
                        };
                        let remote = entry.insert(RemotePeer::new(peer));
                        remote.start_requests_task(remotes.clone(), peer_id.clone());
                        fuchsia_async::spawn(log_result("peer discovery".to_string(), discover_and_start(remote.peer(), profile_svc.clone(), peer_id.clone(), remotes.clone())));
                    }
                }
            }
            ProfileEvent::OnConnected { device_id, service_id: _, channel, protocol } => {
                fx_log_info!("Audio Sink connected from {}: {:?} {:?}", device_id, channel, protocol);
                match remotes.write().entry(device_id.clone()) {
                    Entry::Occupied(mut entry) => {
                        if let Err(e) = entry.get_mut().receive_channel(channel) {
                            fx_log_warn!("{} connected an unexpected channel: {}", device_id, e);
                        }
                    }
                    Entry::Vacant(_) => {
                        fx_log_info!("Peer connected: {}, disconnecting (will connect from search)", device_id);
                        continue;
                    }
                }
            }
        }
    }
    Ok(())
}
