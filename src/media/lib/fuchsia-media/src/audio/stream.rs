// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    bitflags::bitflags,
    fuchsia_syslog::fx_log_info,
    fuchsia_zircon as zx,
    std::{mem, sync::Arc},
};

use super::{AudioCommandHeader, AudioStreamFormatRange, ChannelInner, ChannelResponder};
use crate::types::{AudioSampleFormat, Decodable, Error, Result, TryFrom};

const AUDIO_CMD_HEADER_LEN: usize = mem::size_of::<AudioCommandHeader>();

const GET_FORMATS_MAX_RANGES_PER_RESPONSE: usize = 15;

#[repr(C)]
#[derive(Default)]
struct GetFormatsResponse {
    pad: u32,
    format_range_count: u16,
    first_format_range_index: u16,
    format_ranges: [AudioStreamFormatRange; GET_FORMATS_MAX_RANGES_PER_RESPONSE],
}

transmute_intovec!(GetFormatsResponse);

#[repr(C)]
struct SetFormatRequest {
    frames_per_second: u32,
    sample_format: u32,
    channels: u16,
}

transmute_decodable!(SetFormatRequest);

#[repr(C)]
struct SetFormatResponse {
    status: zx::Status,
    external_delay_nsec: u64,
}

transmute_intovec!(SetFormatResponse);

#[repr(C)]
struct GetGainResponse {
    cur_mute: bool,
    cur_agc: bool,
    cur_gain: f32,
    can_mute: bool,
    can_agc: bool,
    min_gain: f32,
    max_gain: f32,
    gain_step: f32,
}

transmute_intovec!(GetGainResponse);

#[repr(transparent)]
struct SetGainFlags(u32);

#[repr(C)]
struct SetGainRequest {
    flags: SetGainFlags,
    gain: f32,
}

bitflags! {
    #[repr(transparent)]
    struct PlugDetectFlags: u32 {
        const ENABLE_NOTIFICATIONS = 0x4000_0000;
        const DISABLE_NOTIFICATIONS = 0x8000_0000;
    }
}

transmute_decodable!(PlugDetectFlags);

bitflags! {
    #[repr(transparent)]
    struct PlugDetectNotifyFlags: u32 {
        const HARDWIRED = 0x0000_0001;
        const CAN_NOTIFY = 0x0000_0002;
        const PLUGGED = 0x8000_0000;
    }
}

#[repr(C)]
struct PlugDetectResponse {
    flags: PlugDetectNotifyFlags,
    plug_state_time: zx::Time,
}

transmute_intovec!(PlugDetectResponse);

#[repr(transparent)]
struct GetStringRequest(u32);

transmute_decodable!(GetStringRequest);

#[repr(C)]
struct GetStringResponse {
    status: zx::Status,
    id: u32,
    string_len: u32,
    string: [u8; 256 - AUDIO_CMD_HEADER_LEN - mem::size_of::<u32>() * 3],
}

transmute_intovec!(GetStringResponse);

impl GetStringResponse {
    fn build(id: StringId, s: &String) -> GetStringResponse {
        const MAX_STRING_LEN: usize = 256 - AUDIO_CMD_HEADER_LEN - mem::size_of::<u32>() * 3;
        let mut r = GetStringResponse {
            status: zx::Status::OK,
            id: (&id).into(),
            string_len: s.len() as u32,
            string: [0; MAX_STRING_LEN],
        };
        let bytes = s.clone().into_bytes();
        if bytes.len() > MAX_STRING_LEN {
            r.string.copy_from_slice(&bytes[0..MAX_STRING_LEN]);
            r.string_len = MAX_STRING_LEN as u32;
        } else {
            r.string[0..bytes.len()].copy_from_slice(&bytes);
        }
        r
    }
}

decodable_enum! {
    #[derive(Clone)]
    StringId<u32> {
        Manufacturer => 0x8000_0000,
        Product      => 0x8000_0001,
    }
}

decodable_enum! {
    #[derive(Clone)]
    CommandType<u32> {
        GetFormats => 0x1000,
        SetFormat => 0x1001,
        GetGain => 0x1002,
        SetGain => 0x1003,
        PlugDetect => 0x1004,
        GetUniqueId => 0x1005,
        GetString => 0x1006,
    }
}

type Responder = ChannelResponder<Request>;

#[derive(Debug)]
pub(crate) enum Request {
    GetFormats {
        responder: GetFormatsResponder,
    },
    SetFormat {
        frames_per_second: u32,
        sample_format: AudioSampleFormat,
        channels: u16,
        responder: SetFormatResponder,
    },
    GetGain {
        responder: GetGainResponder,
    },
    SetGain {
        responder: SetGainResponder,
    },
    PlugDetect {
        notifications: bool,
        responder: PlugDetectResponder,
    },
    GetUniqueId {
        responder: GetUniqueIdResponder,
    },
    GetString {
        id: StringId,
        responder: GetStringResponder,
    },
}

impl Decodable for Request {
    fn decode(bytes: &[u8]) -> Result<Request> {
        const HEAD_LEN: usize = AUDIO_CMD_HEADER_LEN;
        let header = AudioCommandHeader::decode(bytes)?;
        let cmd_type = CommandType::try_from(header.command_type)?;
        let chan_responder = ChannelResponder::build(header.transaction_id, &cmd_type);
        let res = match cmd_type {
            CommandType::GetFormats => Request::GetFormats {
                responder: GetFormatsResponder(chan_responder),
            },
            CommandType::SetFormat => {
                let request = SetFormatRequest::decode(&bytes[HEAD_LEN..])?;
                Request::SetFormat {
                    responder: SetFormatResponder(chan_responder),
                    frames_per_second: request.frames_per_second,
                    sample_format: AudioSampleFormat::try_from(request.sample_format)?,
                    channels: request.channels,
                }
            }
            CommandType::GetGain => Request::GetGain {
                responder: GetGainResponder(chan_responder),
            },
            CommandType::PlugDetect => {
                let request = PlugDetectFlags::decode(&bytes[HEAD_LEN..])?;
                Request::PlugDetect {
                    notifications: request.contains(PlugDetectFlags::ENABLE_NOTIFICATIONS),
                    responder: PlugDetectResponder(chan_responder),
                }
            }
            CommandType::GetUniqueId => Request::GetUniqueId {
                responder: GetUniqueIdResponder(chan_responder),
            },
            CommandType::GetString => {
                let request = GetStringRequest::decode(&bytes[HEAD_LEN..])?;
                let id = StringId::try_from(request.0)?;
                Request::GetString {
                    id: id.clone(),
                    responder: GetStringResponder { inner: chan_responder, id },
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
            Request::GetFormats { responder } => responder.0.set_channel(channel),
            Request::SetFormat { responder, .. } => responder.0.set_channel(channel),
            Request::GetGain { responder } => responder.0.set_channel(channel),
            Request::SetGain { responder } => responder.0.set_channel(channel),
            Request::PlugDetect { responder, .. } => responder.0.set_channel(channel),
            Request::GetUniqueId { responder } => responder.0.set_channel(channel),
            Request::GetString { responder, .. } => responder.inner.set_channel(channel),
        }
    }
}

#[derive(Debug)]
pub(crate) struct GetFormatsResponder(Responder);

impl GetFormatsResponder {
    pub fn reply(self, supported_formats: Vec<AudioStreamFormatRange>) -> Result<()> {
        // TODO: Merge any format ranges that are compatible?
        let total_formats = supported_formats.len();
        for (chunk_idx, ranges) in
            supported_formats.chunks(GET_FORMATS_MAX_RANGES_PER_RESPONSE).enumerate()
        {
            let first_index = chunk_idx * GET_FORMATS_MAX_RANGES_PER_RESPONSE;
            let mut resp = GetFormatsResponse {
                format_range_count: total_formats as u16,
                first_format_range_index: first_index as u16,
                ..Default::default()
            };
            resp.format_ranges[0..ranges.len()].clone_from_slice(ranges);
            let payload: Vec<u8> = resp.into();
            self.0.send(&payload)?;
        }
        Ok(())
    }
}

#[derive(Debug)]
pub(crate) struct SetFormatResponder(Responder);

impl SetFormatResponder {
    pub fn reply(
        self,
        status: zx::Status,
        external_delay_nsec: u64,
        rb_channel: Option<zx::Channel>,
    ) -> Result<()> {
        let resp = SetFormatResponse { status, external_delay_nsec };
        let payload: Vec<u8> = resp.into();
        let handles: Vec<zx::Handle> = rb_channel.into_iter().map(Into::into).collect();
        self.0.send_with_handles(&payload, handles)
    }
}

#[derive(Debug)]
pub(crate) struct GetGainResponder(Responder);

impl GetGainResponder {
    pub fn reply(
        self,
        mute: Option<bool>,
        agc: Option<bool>,
        gain: f32,
        gain_range: [f32; 2],
        gain_step: f32,
    ) -> Result<()> {
        let resp = GetGainResponse {
            can_mute: mute.is_some(),
            cur_mute: mute.unwrap_or(false),
            can_agc: agc.is_some(),
            cur_agc: agc.unwrap_or(false),
            cur_gain: gain,
            min_gain: gain_range[0],
            max_gain: gain_range[1],
            gain_step,
        };
        let payload: Vec<u8> = resp.into();
        self.0.send(&payload)
    }
}

#[derive(Debug)]
pub(crate) struct SetGainResponder(Responder);

#[derive(Debug)]
pub(crate) struct PlugDetectResponder(Responder);

impl PlugDetectResponder {
    pub fn reply(self, plugged: bool, can_notify: bool, plug_state_time: zx::Time) -> Result<()> {
        let mut flags = PlugDetectNotifyFlags::empty();
        if plugged {
            flags.insert(PlugDetectNotifyFlags::PLUGGED)
        }
        if can_notify {
            flags.insert(PlugDetectNotifyFlags::CAN_NOTIFY)
        }
        let resp = PlugDetectResponse { flags, plug_state_time };
        let payload: Vec<u8> = resp.into();
        self.0.send(&payload)
    }
}

#[derive(Debug)]
pub(crate) struct GetUniqueIdResponder(Responder);

impl GetUniqueIdResponder {
    pub fn reply(self, id: &[u8; 16]) -> Result<()> {
        self.0.send(id)
    }
}

#[derive(Debug)]
pub(crate) struct GetStringResponder {
    id: StringId,
    inner: Responder,
}

impl GetStringResponder {
    pub fn reply(self, string: &String) -> Result<()> {
        let resp = GetStringResponse::build(self.id, string);
        let payload: Vec<u8> = resp.into();
        self.inner.send(&payload)
    }
}
