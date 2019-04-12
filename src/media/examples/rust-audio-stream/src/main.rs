// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]
#![recursion_limit = "1024"]

use {
    fuchsia_media::{self, audio::{Encoder,AudioFrameStream,SoftPcmAudioOutput}},
    fuchsia_syslog::{self, fx_log_info, fx_log_warn},
    fuchsia_async as fasync,
    fidl_fuchsia_media::*,
    fidl_fuchsia_mediacodec::*,
    fuchsia_zircon as zx,
    futures::{channel::mpsc::{self, Sender}, SinkExt, Future, StreamExt, select},
    failure::{Error, ResultExt},
};

async fn throwout_result<F>(f: F)
    where F: Future<Output = Result<(), Error>>
{
    match await!(f) {
        Err(e) => fx_log_warn!("Task ended with error: {:?}", e),
        Ok(()) => fx_log_info!("Task ended"),
    };
}

async fn sbc_encoder_task(mut frames_stream: AudioFrameStream, mut encoded_frames_send: Sender<Vec<u8>>) -> Result<(), Error> {
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

    let mut encoded_output = encoder.take_encoded_stream();

    loop {
        let mut frames_fut = frames_stream.select_next_some();
        let mut encoded_fut = encoded_output.select_next_some();
        select! {
            frame_data = frames_fut => { encoder.deliver_input(&frame_data?)?; },
            encoded = encoded_fut => { await!(encoded_frames_send.send(encoded?))?; },
            complete => return Ok(()),
        };
    }
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["rust-audio-stream"]).expect("Can't init logger");

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

    let (encoded_sender, mut encoded_receiver) = mpsc::channel(5);
    fuchsia_async::spawn(throwout_result(sbc_encoder_task(frame_stream, encoded_sender)));

    fx_log_info!("Starting to loop for the frames");

    //let mut timer = zx::Time::get(zx::ClockId::Monotonic);
    //let second = zx::Duration::from_seconds(1);

    let mut encoded_bytes_count = 0;

    loop {
        let encoded = await!(encoded_receiver.select_next_some());
        encoded_bytes_count += encoded.len();
        fx_log_info!("{} encoded", encoded_bytes_count);
    }
}
