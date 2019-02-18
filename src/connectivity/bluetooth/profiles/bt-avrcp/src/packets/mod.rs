// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    bt_avctp::pub_decodable_enum,
    failure::Fail,
    std::{convert::TryFrom, result, slice::SliceIndex},
};

mod continuation;
mod getcapabilities;
mod getelementattributes;
mod notification;

pub use {
    self::continuation::*, self::getcapabilities::*, self::getelementattributes::*,
    self::notification::*,
};

/// The error types for packet parsing.
#[derive(Fail, Debug, PartialEq)]
pub enum Error {
    /// The value that was sent on the wire was out of range.
    #[fail(display = "Value was out of range")]
    OutOfRange,

    /// The header was invalid.
    #[fail(display = "Invalid header for a message")]
    InvalidHeader,

    /// The body format was invalid.
    #[fail(display = "Failed to parse message contents")]
    InvalidMessage,

    /// The packet is unhandled but not necessarily invalid.
    #[fail(display = "Message is unsupported but not necessarily invalid")]
    UnsupportedMessage,

    /// A message couldn't be encoded.
    #[fail(display = "Encountered an error encoding a message")]
    Encoding,

    #[doc(hidden)]
    #[fail(display = "__Nonexhaustive error should never be created.")]
    __Nonexhaustive,
}

type PacketResult<T> = result::Result<T, Error>;

pub_decodable_enum! {
    MediaAttributeId<u8, Error> {
        Title => 0x1,
        ArtistName => 0x2,
        AlbumName => 0x3,
        TrackNumber => 0x4,
        TotalNumberOfTracks => 0x5,
        Genre => 0x6,
        PlayingTime => 0x7,
        DefaultCoverArt => 0x8,
    }
}

pub_decodable_enum! {
    PduId<u8, Error> {
        GetCapabilities => 0x10,
        ListPlayerApplicationSettingAttributes => 0x11,
        ListPlayerApplicationSettingValues => 0x12,
        GetCurrentPlayerApplicationSettingValue => 0x13,
        SetPlayerApplicationSettingValue => 0x14,
        GetPlayerApplicationSettingAttributeText => 0x15,
        GetPlayerApplicationSettingValueText => 0x16,
        InformDisplayableCharacterSet => 0x17,
        InformBatteryStatusOfCT => 0x18,
        GetElementAttributes => 0x20,
        GetPlayStatus => 0x30,
        RegisterNotification => 0x31,
        RequestContinuingResponse => 0x40,
        AbortContinuingResponse => 0x41,
        SetAbsoluteVolume => 0x50,
        SetAddressedPlayer => 0x60,
        PlayItem => 0x74,
        AddToNowPlaying => 0x90,
    }
}

pub_decodable_enum! {
    PacketType<u8, Error> {
        Single => 0b00,
        Start => 0b01,
        Continue => 0b10,
        Stop => 0b11,
    }
}

// Copied from the AVCTP crate. They need to be local types so that the impls work.
/// A decodable type can be created from a byte buffer.
/// The type returned is separate (copied) from the buffer once decoded.
pub trait Decodable<E = Error>: Sized {
    /// Decodes into a new object, or returns an error.
    fn decode(buf: &[u8]) -> result::Result<Self, E>;
}

/// A encodable type can write itself into a byte buffer.
pub trait Encodable<E = Error>: Sized {
    /// Returns the number of bytes necessary to encode |self|
    fn encoded_len(&self) -> usize;

    /// Writes the encoded version of |self| at the start of |buf|
    /// |buf| must be at least size() length.
    fn encode(&self, buf: &mut [u8]) -> result::Result<(), E>;
}

pub struct VendorDependentPreamble {
    pub pdu_id: u8,
    pub packet_type: PacketType,
    pub parameter_length: u16,
}

impl VendorDependentPreamble {
    pub fn new(
        pdu_id: u8,
        packet_type: PacketType,
        parameter_length: u16,
    ) -> VendorDependentPreamble {
        VendorDependentPreamble { pdu_id, packet_type, parameter_length }
    }

    pub fn new_single(pdu_id: u8, parameter_length: u16) -> VendorDependentPreamble {
        Self::new(pdu_id, PacketType::Single, parameter_length)
    }

    pub fn packet_type(&self) -> PacketType {
        self.packet_type
    }
}

impl Decodable for VendorDependentPreamble {
    fn decode(buf: &[u8]) -> PacketResult<Self> {
        if buf.len() < 4 {
            return Err(Error::InvalidMessage);
        }
        Ok(Self {
            pdu_id: buf[0],
            packet_type: PacketType::try_from(buf[1])?,
            parameter_length: ((buf[2] as u16) << 8) | buf[3] as u16,
        })
    }
}

impl Encodable for VendorDependentPreamble {
    fn encoded_len(&self) -> usize {
        4
    }

    fn encode(&self, buf: &mut [u8]) -> PacketResult<()> {
        buf[0] = self.pdu_id;
        buf[1] = u8::from(&self.packet_type);
        buf[2] = (self.parameter_length >> 8) as u8;
        buf[3] = (self.parameter_length & 0xff) as u8;
        Ok(())
    }
}

pub trait VendorDependent: Encodable {
    /// Protocol Data Unit type.
    fn pdu_id(&self) -> PduId;

    /// Encode packet for single command/response
    fn encode_packet(&self) -> Result<Vec<u8>, Error> {
        let len = self.encoded_len();
        let preamble = VendorDependentPreamble::new_single(u8::from(&self.pdu_id()), len as u16);
        let prelen = preamble.encoded_len();
        let mut buf = vec![0; len + prelen];
        preamble.encode(&mut buf[..])?;
        self.encode(&mut buf[prelen..])?;
        Ok(buf)
    }

    const AVC_PAYLOAD_SIZE: usize = 508; // 512 - 4 byte preamble

    /// Encode packet for potential multiple responses
    fn encode_packets(&self) -> Result<Vec<Vec<u8>>, Error> {
        let mut buf = vec![0; self.encoded_len()];
        self.encode(&mut buf[..])?;

        let mut payloads = vec![];
        let mut len_remaining = self.encoded_len();
        let mut packet_type = if len_remaining > Self::AVC_PAYLOAD_SIZE {
            PacketType::Start
        } else {
            PacketType::Single
        };
        let mut offset = 0;

        loop {
            // length - preamble size
            let current_len = if len_remaining > Self::AVC_PAYLOAD_SIZE {
                Self::AVC_PAYLOAD_SIZE
            } else {
                len_remaining
            };
            let preamble = VendorDependentPreamble::new(
                u8::from(&self.pdu_id()),
                packet_type,
                current_len as u16,
            );

            let mut payload_buf = vec![0; preamble.encoded_len()];
            preamble.encode(&mut payload_buf[..])?;
            payload_buf.extend_from_slice(&buf[offset..current_len + offset]);
            payloads.push(payload_buf);

            len_remaining -= current_len;
            offset += current_len;
            if len_remaining == 0 {
                break;
            } else if len_remaining <= Self::AVC_PAYLOAD_SIZE {
                packet_type = PacketType::Stop;
            } else {
                packet_type = PacketType::Continue;
            }
        }
        Ok(payloads)
    }
}

/// For sending raw preambled packets
pub struct RawVendorDependentPacket {
    pdu_id: PduId,
    payload: Vec<u8>,
}

impl RawVendorDependentPacket {
    pub fn new(pdu_id: PduId, payload: &[u8]) -> Self {
        Self { pdu_id, payload: payload.to_vec() }
    }
}

impl Encodable for RawVendorDependentPacket {
    fn encoded_len(&self) -> usize {
        self.payload.len()
    }

    fn encode(&self, buf: &mut [u8]) -> PacketResult<()> {
        if buf.len() != self.payload.len() {
            return Err(Error::OutOfRange);
        }

        buf.copy_from_slice(&self.payload[..]);
        Ok(())
    }
}

impl VendorDependent for RawVendorDependentPacket {
    fn pdu_id(&self) -> PduId {
        self.pdu_id
    }
}

trait FillExt<T> {
    fn fill(&mut self, v: T);
}

impl FillExt<u8> for [u8] {
    fn fill(&mut self, v: u8) {
        for i in self {
            *i = v
        }
    }
}

/// Helper to make it easier to read from a byte buffer without having to do range checks.
trait BuffHelper<T: ?Sized> {
    fn fetch<I>(&self, index: I) -> Result<&I::Output, Error>
    where
        I: SliceIndex<Self>;
}

impl BuffHelper<u8> for [u8] {
    fn fetch<I>(&self, index: I) -> Result<&I::Output, Error>
    where
        I: SliceIndex<Self>,
    {
        index.get(self).ok_or(Error::OutOfRange)
    }
}
