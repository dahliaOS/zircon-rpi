// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fuchsia_syslog::fx_log_info,
    fuchsia_zircon as zx,
    fuchsia_zircon::HandleBased,
    futures::task::Waker,
    std::{mem, sync::Arc},
};

use super::{AudioCommandHeader, ChannelInner, ChannelResponder};
use crate::types::{AudioSampleFormat, Decodable, Error, Result, TryFrom};

const AUDIO_CMD_HEADER_LEN: usize = mem::size_of::<AudioCommandHeader>();

#[repr(C)]
struct GetFifoDepthResponse {
    result: zx::Status,
    fifo_depth: u32,
}

transmute_intovec!(GetFifoDepthResponse);

#[repr(C)]
struct GetBufferRequest {
    min_ring_buffer_frames: u32,
    notifications_per_ring: u32,
}

transmute_decodable!(GetBufferRequest);

#[repr(C)]
struct GetBufferResponse {
    result: zx::Status,
    num_ring_buffer_frames: u32,
}

transmute_intovec!(GetBufferResponse);

#[repr(C)]
struct StartResponse {
    result: zx::Status,
    start_time: u64,
}

transmute_intovec!(StartResponse);

#[repr(C)]
struct StopResponse {
    result: zx::Status,
}

transmute_intovec!(StopResponse);

decodable_enum! {
    #[derive(Clone)]
    CommandType<u32> {
        GetFifoDepth => 0x3000,
        GetBuffer => 0x3001,
        Start => 0x3002,
        Stop => 0x3003,
    }
}

type Responder = ChannelResponder<Request>;

#[derive(Debug)]
pub(crate) enum Request {
    GetFifoDepth {
        responder: GetFifoDepthResponder,
    },
    GetBuffer {
        min_ring_buffer_frames: u32,
        notifications_per_ring: u32,
        responder: GetBufferResponder,
        // TODO: maybe we should have the NotificationResponder here?
    },
    Start {
        responder: StartResponder,
    },
    Stop {
        responder: StopResponder,
    },
}

impl Decodable for Request {
    fn decode(bytes: &[u8]) -> Result<Request> {
        const HEAD_LEN: usize = AUDIO_CMD_HEADER_LEN;
        let header = AudioCommandHeader::decode(bytes)?;
        let cmd_type = CommandType::try_from(header.command_type)?;
        let chan_responder = ChannelResponder::build(header.transaction_id, &cmd_type);
        let res = match cmd_type {
            CommandType::Start => Request::Start {
                responder: StartResponder(chan_responder),
            },
            CommandType::GetFifoDepth => Request::GetFifoDepth {
                responder: GetFifoDepthResponder(chan_responder),
            },
            CommandType::GetBuffer => {
                let r = GetBufferRequest::decode(&bytes[HEAD_LEN..])?;
                Request::GetBuffer {
                    min_ring_buffer_frames: r.min_ring_buffer_frames,
                    notifications_per_ring: r.notifications_per_ring,
                    responder: GetBufferResponder(chan_responder),
                }
            }
            _ => {
                fx_log_info!(
                    "Unimplemented message: id {} type 0x{:X}",
                    header.transaction_id,
                    header.command_type
                );
                return Err(Error::UnimplementedMessage);
            }
        };
        Ok(res)
    }
}

impl Request {
    pub fn set_responder_channel(&mut self, channel: Arc<ChannelInner<Request>>) {
        match self {
            Request::GetFifoDepth { responder } => responder.0.set_channel(channel),
            Request::GetBuffer { responder, .. } => responder.0.set_channel(channel),
            Request::Start { responder } => responder.0.set_channel(channel),
            Request::Stop { responder } => responder.0.set_channel(channel),
        }
    }
}

#[derive(Debug)]
pub(crate) struct GetFifoDepthResponder(Responder);

impl GetFifoDepthResponder {
    pub fn reply(self, result: zx::Status, fifo_depth: u32) -> Result<()> {
        let resp = GetFifoDepthResponse { result, fifo_depth };
        let payload: Vec<u8> = resp.into();
        self.0.send(&payload)
    }
}

#[derive(Debug)]
pub(crate) struct GetBufferResponder(Responder);

impl GetBufferResponder {
    pub fn reply(
        self,
        result: zx::Status,
        num_ring_buffer_frames: u32,
        vmo: Option<zx::Handle>,
    ) -> Result<()> {
        let resp = GetBufferResponse { result, num_ring_buffer_frames };
        let payload: Vec<u8> = resp.into();
        self.0.send_with_handles(&payload, vmo.into_iter().collect())
    }
}

#[derive(Debug)]
pub(crate) struct StartResponder(Responder);

impl StartResponder {
    pub fn reply(self, result: zx::Status, start_time: u64) -> Result<()> {
        let resp = StartResponse { result, start_time };
        let payload: Vec<u8> = resp.into();
        self.0.send(&payload)
    }
}

#[derive(Debug)]
pub(crate) struct StopResponder(Responder);

impl StopResponder {
    pub fn reply(self, result: zx::Status) -> Result<()> {
        let resp = StopResponse { result };
        let payload: Vec<u8> = resp.into();
        self.0.send(&payload)
    }
}

/// A FrameVmo wraps a VMO with time tracking.  When a FrameVmo is started, it
/// assumes that audio frame data is being written to the VMO at the rate specifie
/// in the format it is set to.  Frames that represent a time range can be
/// retrieved from the buffer.
pub(crate) struct FrameVmo {
    /// Ring Buffer VMO. Size zero until the ringbuffer is established.  Shared with
    /// the AudioFrameStream given back to the client.
    vmo: zx::Vmo,

    /// Cached size of the ringbuffer, in bytes.  Used to avoid zx_get_size() syscalls.
    size: usize,

    /// The time that streaming was started.
    /// Used to calculate the currently available frames.
    /// None if the stream is not started.
    start_time: Option<zx::Time>,

    /// This waker will be woken if we are started.
    wake_on_start: Option<Waker>,

    /// The number of frames per second.
    frames_per_second: u32,

    /// The audio format of the frames.
    format: Option<AudioSampleFormat>,

    /// The number of channels.
    channels: u16,
}

impl FrameVmo {
    pub(crate) fn new() -> Result<FrameVmo> {
        Ok(FrameVmo {
            vmo: zx::Vmo::create(0).map_err(|e| Error::IOError(e))?,
            size: 0,
            start_time: None,
            wake_on_start: None,
            frames_per_second: 0,
            format: None,
            channels: 0,
        })
    }

    /// Set the format of this buffer.   Returns a handle representing the VMO.
    /// `frames` is the number of frames the VMO should be able to hold.
    pub(crate) fn set_format(
        &mut self,
        frames_per_second: u32,
        format: AudioSampleFormat,
        channels: u16,
        frames: usize,
    ) -> Result<zx::Vmo> {
        if self.start_time.is_some() {
            return Err(Error::InvalidState);
        }
        let new_size = format.compute_frame_size(channels as usize)? * frames;
        self.vmo = zx::Vmo::create(new_size as u64).map_err(|e| Error::IOError(e))?;
        self.size = new_size;
        self.format = Some(format);
        self.frames_per_second = frames_per_second;
        self.channels = channels;
        Ok(self.vmo.duplicate_handle(zx::Rights::SAME_RIGHTS).map_err(|e| Error::IOError(e))?)
    }

    pub(crate) fn set_start_waker(&mut self, lw: Waker) {
        self.wake_on_start = Some(lw);
    }

    /// Start the audio clock for the buffer at `time`
    pub(crate) fn start(&mut self, time: zx::Time) -> Result<()> {
        if self.start_time.is_some() || self.format.is_none() {
            return Err(Error::InvalidState);
        }
        self.start_time = Some(time);
        if let Some(waker) = self.wake_on_start.take() {
            fx_log_info!("ringing the start waker");
            waker.wake();
        }
        Ok(())
    }

    pub(crate) fn start_time(&self) -> Option<zx::Time> {
        self.start_time
    }

    /// Stop the audio clock in tbe buffer.
    /// This operation cannot fail, and is idempotent.
    pub(crate) fn stop(&mut self) {
        self.start_time = None;
    }

    /// Retrieve the complete frames available from `from` to `until`
    /// Frames that end after `from` and before `until` are included.
    /// `until` is not allowed to be in the future.
    /// Returns a vector of bytes represeting the frames and a number of frames
    /// that are missing due to being unavailable.
    pub(crate) fn get_frames(
        &mut self,
        mut from: zx::Time,
        until: zx::Time,
    ) -> Result<(Vec<u8>, usize)> {
        if self.start_time.is_none() {
            return Err(Error::InvalidState);
        }
        let now = zx::Time::get(zx::ClockId::Monotonic);
        if until > now {
            fx_log_info!("Can't get frames from the future");
            return Err(Error::OutOfRange);
        }
        let start_time = self.start_time.clone().unwrap();
        if from < start_time {
            fx_log_info!("Can't get frames from before start");
            return Err(Error::OutOfRange);
        }

        let vmo_frames = self.size / self.bytes_per_frame();

        let oldest_frame_start = now - self.duration_from_frames(vmo_frames as i64);

        if until <= oldest_frame_start {
            // every frame from this time period is missing
            return Ok((vec![], self.frames_from_duration(until - from) as usize));
        }

        let missing_frames = if oldest_frame_start < from {
            0
        } else {
            self.frames_from_duration(oldest_frame_start - from) as usize
        };

        if missing_frames > 0 {
            from = oldest_frame_start;
        }

        // Start time is the zero frame.
        let mut frame_from_idx = self.frames_before(from) % vmo_frames;
        let mut frame_until_idx = self.frames_before(until) % vmo_frames;

        if frame_from_idx == frame_until_idx {
            return Ok((vec![], missing_frames));
        }

        if frame_from_idx > frame_until_idx {
            frame_until_idx += vmo_frames;
        }

        let frames_available = frame_until_idx - frame_from_idx;
        let bytes_available = frames_available * self.bytes_per_frame();
        let mut out_vec = vec![0; bytes_available];

        let mut ndx = 0;

        if frame_until_idx > vmo_frames {
            let frames_to_read = vmo_frames - frame_from_idx;
            let bytes_to_read = frames_to_read * self.bytes_per_frame();
            let byte_start = frame_from_idx * self.bytes_per_frame();
            self.vmo
                .read(&mut out_vec[0..bytes_to_read], byte_start as u64)
                .map_err(|e| Error::IOError(e))?;
            frame_from_idx = 0;
            frame_until_idx -= vmo_frames;
            ndx = bytes_to_read;
        }

        let frames_to_read = frame_until_idx - frame_from_idx;
        let bytes_to_read = frames_to_read * self.bytes_per_frame();
        let byte_start = frame_from_idx * self.bytes_per_frame();

        self.vmo
            .read(&mut out_vec[ndx..ndx + bytes_to_read], byte_start as u64)
            .map_err(|e| Error::IOError(e))?;

        Ok((out_vec, missing_frames))
    }

    /// Count of the number of frames that have occurred before `time`.
    fn frames_before(&self, time: zx::Time) -> usize {
        if self.start_time().is_none() {
            return 0;
        }
        let start_time = self.start_time.clone().unwrap();
        if time < start_time {
            return 0;
        }
        return self.frames_from_duration(time - start_time) as usize;
    }

    /// Open front, closed end frames from duration.
    /// This means if duration is an exact duration of a number of frames, the last
    /// frame will be considered to not be inside the duration, and will not be counted.
    fn frames_from_duration(&self, duration: zx::Duration) -> i64 {
        assert!(
            duration >= zx::Duration::from_nanos(0),
            "frames_from_duration is not defined for negative durations"
        );
        if duration == zx::Duration::from_nanos(0) {
            return 0;
        }
        let mut frames = (duration.into_nanos() / 1_000_000_000) * self.frames_per_second as i64;
        let mut frames_partial =
            ((duration.into_nanos() % 1_000_000_000) as f64 / 1e9) * self.frames_per_second as f64;
        if frames_partial.ceil() == frames_partial {
            // The end of this frame is exactly on the duration, and we are doing a
            frames_partial -= 1.0;
        }
        frames += frames_partial as i64;
        frames
    }

    /// Return an amount of time that guarantees that `frames` frames has passed.
    /// This means that partial nanoseconds will be rounded up, so that
    /// [time, time + duration_from_frames(n)] is guaranteed to include n audio frames.
    /// Only well-defined for positive nnumbers of frames.
    fn duration_from_frames(&self, frames: i64) -> zx::Duration {
        assert!(frames > 0, "duration_from_trames is not defined for negative amounts");
        let secs = frames / self.frames_per_second as i64;
        let nanos_per_frame: f64 = 1e9 / self.frames_per_second as f64;
        let nanos =
            ((frames % self.frames_per_second as i64) as f64 * nanos_per_frame).ceil() as i64;
        zx::Duration::from_nanos((secs * 1_000_000_000) + nanos)
    }

    pub(crate) fn next_frame_after(&self, time: zx::Time) -> Result<zx::Time> {
        if self.start_time.is_none() {
            return Err(Error::InvalidState);
        }
        let start_time = self.start_time.clone().unwrap();
        let frames =
            if time <= start_time { 0 } else { self.frames_from_duration(time - start_time) };
        Ok(start_time + self.duration_from_frames(frames + 1))
    }

    fn bytes_per_frame(&self) -> usize {
        self.format
            .as_ref()
            .expect("need format")
            .compute_frame_size(self.channels as usize)
            .expect("unknown format size")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Convenient becausew one byte = one frame.
    const TEST_FORMAT: AudioSampleFormat = AudioSampleFormat::Eight { unsigned: false };
    const TEST_CHANNELS: u16 = 1;
    const TEST_FPS: u32 = 48000;

    // At 48kHz, each frame is 20833 and 1/3 nanoseconds. We add one nanosecond
    // because we always overestimate durations.
    const ONE_FRAME_NANOS: i64 = 20833 + 1;
    const TWO_FRAME_NANOS: i64 = 20833 * 2 + 1;
    const THREE_FRAME_NANOS: i64 = 20833 * 3 + 1;

    fn get_test_vmo(frames: usize) -> FrameVmo {
        let mut vmo = FrameVmo::new().expect("can't make a framevmo");
        let _handle = vmo.set_format(TEST_FPS, TEST_FORMAT, TEST_CHANNELS, frames).unwrap();
        vmo
    }

    #[test]
    fn test_bytes_per_frame() {
        let vmo = get_test_vmo(5);

        assert_eq!(1, vmo.bytes_per_frame());

        let mut vmo = FrameVmo::new().expect("can't make a framevmo");
        let format = AudioSampleFormat::Sixteen { unsigned: false, invert_endian: false };
        let _handle = vmo.set_format(TEST_FPS, format, 2, 2).unwrap();

        assert_eq!(4, vmo.bytes_per_frame());
    }

    #[test]
    fn test_duration_from_frames() {
        let vmo = get_test_vmo(5);

        assert_eq!(zx::Duration::from_nanos(ONE_FRAME_NANOS), vmo.duration_from_frames(1));
        assert_eq!(zx::Duration::from_nanos(TWO_FRAME_NANOS), vmo.duration_from_frames(2));
        assert_eq!(zx::Duration::from_nanos(THREE_FRAME_NANOS), vmo.duration_from_frames(3));

        assert_eq!(zx::Duration::from_seconds(1), vmo.duration_from_frames(TEST_FPS as i64));

        assert_eq!(zx::Duration::from_millis(1500), vmo.duration_from_frames(72000));
    }

    #[test]
    fn test_frames_from_duration() {
        let vmo = get_test_vmo(5);

        assert_eq!(0, vmo.frames_from_duration(zx::Duration::from_nanos(0)));

        assert_eq!(0, vmo.frames_from_duration(zx::Duration::from_nanos(ONE_FRAME_NANOS - 1)));
        assert_eq!(1, vmo.frames_from_duration(zx::Duration::from_nanos(ONE_FRAME_NANOS)));

        // Three frames is an exact number of nanoseconds, so it should only count if we provide
        // a duration that is LONGER.
        assert_eq!(2, vmo.frames_from_duration(zx::Duration::from_nanos(THREE_FRAME_NANOS - 1)));
        assert_eq!(2, vmo.frames_from_duration(zx::Duration::from_nanos(THREE_FRAME_NANOS)));
        assert_eq!(3, vmo.frames_from_duration(zx::Duration::from_nanos(THREE_FRAME_NANOS + 1)));

        assert_eq!(TEST_FPS as i64 - 1, vmo.frames_from_duration(zx::Duration::from_millis(1000)));
        assert_eq!(72000 - 1, vmo.frames_from_duration(zx::Duration::from_millis(1500)));

        assert_eq!(10660, vmo.frames_from_duration(zx::Duration::from_nanos(222084000)));
    }

    fn get_time_now() -> zx::Time {
        zx::Time::get(zx::ClockId::Monotonic)
    }

    #[test]
    fn test_next_frame_after() {
        let mut vmo = get_test_vmo(5);

        assert_eq!(Err(Error::InvalidState), vmo.next_frame_after(get_time_now()));

        let start_time = get_time_now();
        assert_eq!(Ok(()), vmo.start(start_time));

        // At 48kHz, each frame is 20833 and 1/3 nanoseconds.
        let first_frame_after = start_time + zx::Duration::from_nanos(ONE_FRAME_NANOS);

        assert_eq!(Ok(first_frame_after), vmo.next_frame_after(start_time));
        assert_eq!(Ok(first_frame_after), vmo.next_frame_after(zx::Time::INFINITE_PAST));

        let next_frame = start_time + zx::Duration::from_nanos(TWO_FRAME_NANOS);
        assert_eq!(Ok(next_frame), vmo.next_frame_after(first_frame_after));

        let one_sec = start_time + zx::Duration::from_seconds(1);
        assert_eq!(Ok(one_sec), vmo.next_frame_after(one_sec - zx::Duration::from_nanos(1)));
    }

    #[test]
    fn test_start_stop() {
        let mut vmo = get_test_vmo(5);

        let start_time = get_time_now();
        assert_eq!(Ok(()), vmo.start(start_time));
        assert_eq!(Err(Error::InvalidState), vmo.start(start_time));

        vmo.stop();

        assert_eq!(Ok(()), vmo.start(start_time));
    }

    fn test_frames_before_exact(
        vmo: &mut FrameVmo,
        time_nanos: i64,
        dur_nanos: i64,
        frames: usize,
    ) {
        vmo.stop();
        let start_time = zx::Time::from_nanos(time_nanos);
        assert_eq!(Ok(()), vmo.start(start_time));
        assert_eq!(frames, vmo.frames_before(start_time + zx::Duration::from_nanos(dur_nanos)));
    }

    #[test]
    fn test_frames_before() {
        let mut vmo = get_test_vmo(5);

        let start_time = get_time_now();
        assert_eq!(Ok(()), vmo.start(start_time));

        assert_eq!(0, vmo.frames_before(start_time));

        assert_eq!(1, vmo.frames_before(start_time + zx::Duration::from_nanos(ONE_FRAME_NANOS)));
        assert_eq!(2, vmo.frames_before(start_time + zx::Duration::from_nanos(THREE_FRAME_NANOS)));
        assert_eq!(
            3,
            vmo.frames_before(start_time + zx::Duration::from_nanos(THREE_FRAME_NANOS + 1))
        );

        assert_eq!(
            TEST_FPS as usize / 4 - 1,
            vmo.frames_before(start_time + zx::Duration::from_millis(250))
        );

        let three_quarters_dur = zx::Duration::from_millis(375);
        assert_eq!(17999, 3 * TEST_FPS as usize / 8 - 1);
        assert_eq!(
            3 * TEST_FPS as usize / 8 - 1,
            vmo.frames_before(start_time + three_quarters_dur)
        );

        assert_eq!(10521, vmo.frames_before(start_time + zx::Duration::from_nanos(219188000)));

        test_frames_before_exact(&mut vmo, 273533747037, 219188000, 10521);
        test_frames_before_exact(&mut vmo, 714329925362, 219292000, 10526);
    }

    #[test]
    fn test_get_frames() {
        let frames = TEST_FPS as usize / 2;
        let mut vmo = get_test_vmo(frames);

        let start_time = get_time_now();
        assert_eq!(Ok(()), vmo.start(start_time));

        let half_dur = zx::Duration::from_millis(250);
        // TODO: Control time so we don't need to sleep like this.
        half_dur.clone().sleep();

        let res = vmo.get_frames(start_time, start_time + half_dur);
        assert!(res.is_ok());
        let (bytes, missed) = res.unwrap();

        assert_eq!(0, missed);
        assert_eq!(frames / 2 - 1, bytes.len());

        // Each `dur` period should pseudo-fill half the vmo.
        // After 750 ms, we should have the oildest frame half-way through
        // the buffer.
        half_dur.clone().sleep();
        half_dur.clone().sleep();

        // We should be able to get some frames that span the end of the buffer
        let three_quarters_dur = zx::Duration::from_millis(375);
        let res = vmo.get_frames(
            start_time + three_quarters_dur,
            start_time + three_quarters_dur + half_dur,
        );
        assert!(res.is_ok());
        let (bytes, missed) = res.unwrap();
        assert_eq!(0, missed);
        assert_eq!(frames / 2, bytes.len());

        // We should also be able to ask for a set of frames that is all located before the
        // oldest point.
        // This should be from about a quarter in to halfway in.
        let res =
            vmo.get_frames(start_time + three_quarters_dur + half_dur, start_time + (half_dur * 3));
        assert!(res.is_ok());
        let (bytes, missed) = res.unwrap();
        assert_eq!(0, missed);
        assert_eq!(frames / 4, bytes.len());
    }

    #[test]
    fn test_multibyte_get_frames() {
        let mut vmo = FrameVmo::new().expect("can't make a framevmo");
        let format = AudioSampleFormat::Sixteen { unsigned: false, invert_endian: false };
        let frames = TEST_FPS as usize / 2;
        let _handle = vmo.set_format(TEST_FPS, format, 2, frames).unwrap();

        let start_time = get_time_now();
        assert_eq!(Ok(()), vmo.start(start_time));

        let half_dur = zx::Duration::from_millis(250);

        half_dur.clone().sleep();

        let res = vmo.get_frames(start_time, start_time + half_dur);
        assert!(res.is_ok());
        let (bytes, missed) = res.unwrap();

        assert_eq!(0, missed);
        // There should be frames / 2 frames here, with 4 bytes per frame.
        assert_eq!(frames * 2 - 4, bytes.len())
    }

    #[test]
    fn test_get_frames_boundaries() {
        let frames = TEST_FPS as usize / 2;
        let mut vmo = get_test_vmo(frames);

        let start_time = get_time_now();
        assert_eq!(Ok(()), vmo.start(start_time));

        let half_dur = zx::Duration::from_millis(250);

        half_dur.clone().sleep();

        let res = vmo.get_frames(
            start_time + zx::Duration::from_nanos(THREE_FRAME_NANOS),
            start_time + zx::Duration::from_nanos(THREE_FRAME_NANOS + 1),
        );
        assert!(res.is_ok(), "result was not ok: {:?}", res);
        let (bytes, missed) = res.unwrap();
        assert_eq!(0, missed);
        assert_eq!(1, bytes.len());

        let res = vmo.get_frames(
            start_time + zx::Duration::from_nanos(3999 * THREE_FRAME_NANOS - ONE_FRAME_NANOS),
            start_time + zx::Duration::from_nanos(3999 * THREE_FRAME_NANOS),
        );
        assert!(res.is_ok(), "result was not ok: {:?}", res);
        let (bytes, missed) = res.unwrap();
        assert_eq!(0, missed);
        assert_eq!(1, bytes.len());

        let mut all_frames_len = 0;
        let mut total_duration = zx::Duration::from_nanos(0);

        for moment in 0..250_000 {
            let start = start_time + zx::Duration::from_nanos(moment * 1000);
            let end = start_time + zx::Duration::from_nanos((moment + 1) * 1000);
            let res = vmo.get_frames(start, end);
            total_duration += zx::Duration::from_nanos(1000);
            assert!(res.is_ok(), "result was not ok: {:?}", res);
            let (bytes, missed) = res.unwrap();
            assert_eq!(0, missed);
            all_frames_len += bytes.len();
            //assert_eq!(all_frames_len, vmo.frames_before(end),
            //           "frame miscount at {:?} + {:?})", start_time, total_duration);
        }

        assert_eq!(zx::Duration::from_millis(250), total_duration);
        assert_eq!(frames / 2 - 1, all_frames_len);
    }
}
