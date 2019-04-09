// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::Fail,
    fuchsia_zircon as zx,
    futures::{
        stream::{FusedStream, Stream},
        task::{Context, Poll},
    },
    std::{pin::Pin, result},
};

/// Result type alias for brevity.
pub type Result<T> = result::Result<T, Error>;

/// The Error type of the fuchsia-media library
#[derive(Fail, Debug, PartialEq)]
pub enum Error {
    /// The value that was received was out of range
    #[fail(display = "Value was out of range")]
    OutOfRange,

    /// The header was invalid when parsing a message.
    #[fail(display = "Invalid Header for a message")]
    InvalidHeader,

    /// Can't encode into a buffer
    #[fail(display = "Encoding error")]
    Encoding,

    /// Encountered an IO error reading
    #[fail(display = "Encountered an IO error reading from the channel: {}", _0)]
    PeerRead(#[cause] zx::Status),

    /// Encountered an IO error writing
    #[fail(display = "Encountered an IO error writing to the channel: {}", _0)]
    PeerWrite(#[cause] zx::Status),

    /// Other IO Error
    #[fail(display = "Encountered an IO error: {}", _0)]
    IOError(#[cause] zx::Status),

    /// Action tried in an invalid state
    #[fail(display = "Tried to do an action in an invalid state")]
    InvalidState,

    /// Responder doesn't have a channel
    #[fail(display = "No channel found for reply")]
    NoChannel,

    /// When a message hasn't been implemented yet, the parser will return this.
    #[fail(display = "Message has not been implemented yet")]
    UnimplementedMessage,

    #[doc(hidden)]
    #[fail(display = "__Nonexhaustive error should never be created.")]
    __Nonexhaustive,
}

pub(crate) trait TryFrom<T>: Sized {
    fn try_from(value: T) -> Result<Self>;
}

/// A decodable type can be created from a byte buffer.
/// The type returned is separate (copied) from the buffer once decoded.
pub(crate) trait Decodable: Sized {
    /// Decodes into a new object, or returns an error.
    fn decode(buf: &[u8]) -> Result<Self>;
}

/// A encodable type can write itself into a byte buffer.
pub(crate) trait Encodable: Sized {
    /// Returns the number of bytes necessary to encode |self|
    fn encoded_len(&self) -> usize;

    /// Writes the encoded version of |self| at the start of |buf|
    /// |buf| must be at least size() length.
    fn encode(&self, buf: &mut [u8]) -> Result<()>;
}

macro_rules! transmute_decodable {
    ($name:ty) => {
        impl Decodable for $name {
            fn decode(buf: &[u8]) -> Result<Self> {
                const LEN: usize = mem::size_of::<$name>();
                if buf.len() < LEN {
                    return Err(Error::Encoding);
                }
                let mut req_ar = [0; LEN];
                req_ar.copy_from_slice(&buf[0..LEN]);
                let res = unsafe { mem::transmute::<[u8; LEN], $name>(req_ar) };
                Ok(res)
            }
        }
    };
}

macro_rules! transmute_intovec {
    ($name:ty) => {
        impl From<$name> for Vec<u8> {
            fn from(v: $name) -> Vec<u8> {
                const LEN: usize = mem::size_of::<$name>();
                let ar = unsafe { mem::transmute::<$name, [u8; LEN]>(v) };
                ar.to_vec()
            }
        }
    };
}

/// Generates an enum value where each variant can be converted into a constant in the given
/// raw_type.  For example:
/// decodable_enum! {
///     Color<u8> {
///        Red => 1,
///        Blue => 2,
///        Green => 3,
///     }
/// }
/// Then Color::try_from(2) returns Color::Red, and u8::from(Color::Red) returns 1.
macro_rules! decodable_enum {
    ($(#[$meta:meta])* $name:ident<$raw_type:ty> {
        $($variant:ident => $val:expr),*,
    }) => {
        $(#[$meta])*
        #[derive(Debug, PartialEq)]
        pub(crate) enum $name {
            $($variant),*
        }

        tofrom_decodable_enum! {
            $name<$raw_type> {
                $($variant => $val),*,
            }
        }
    }
}

/*
/// The same as decodable_enum, but the struct is public.
macro_rules! pub_decodable_enum {
    ($(#[$meta:meta])* $name:ident<$raw_type:ty> {
        $($variant:ident => $val:expr),*,
    }) => {
        $(#[$meta])*
        #[derive(Debug, PartialEq)]
        pub enum $name {
            $($variant),*
        }

        tofrom_decodable_enum! {
            $name<$raw_type> {
                $($variant => $val),*,
            }
        }
    }
}
*/

/// A From<&$name> for $raw_type implementation and
/// TryFrom<$raw_type> for $name implementation, used by (pub_)decodable_enum
macro_rules! tofrom_decodable_enum {
    ($name:ident<$raw_type:ty> {
        $($variant:ident => $val:expr),*,
    }) => {
        impl From<&$name> for $raw_type {
            fn from(v: &$name) -> $raw_type {
                match v {
                    $($name::$variant => $val),*,
                }
            }
        }

        impl From<$name> for $raw_type {
            fn from(v: $name) -> $raw_type {
                match v {
                    $($name::$variant => $val),*,
                }
            }
        }

        impl TryFrom<$raw_type> for $name {
            fn try_from(value: $raw_type) -> Result<Self> {
                match value {
                    $($val => Ok($name::$variant)),*,
                    _ => Err(Error::OutOfRange),
                }
            }
        }
    }
}

pub(crate) struct MaybeStream<T: Stream>(Option<T>);

impl<T: Stream + Unpin> MaybeStream<T> {
    pub(crate) fn set(&mut self, stream: T) {
        self.0 = Some(stream)
    }

    fn poll_next(&mut self, cx: &mut Context<'_>) -> Poll<Option<T::Item>> {
        Pin::new(self.0.as_mut().unwrap()).poll_next(cx)
    }
}

impl<T: Stream> Default for MaybeStream<T> {
    fn default() -> Self {
        MaybeStream(None)
    }
}

impl<T: Stream + Unpin> Stream for MaybeStream<T> {
    type Item = T::Item;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        if self.0.is_none() {
            return Poll::Pending;
        }
        self.get_mut().poll_next(cx)
    }
}

impl<T: FusedStream + Stream> FusedStream for MaybeStream<T> {
    fn is_terminated(&self) -> bool {
        if self.0.is_none() {
            false
        } else {
            self.0.as_ref().unwrap().is_terminated()
        }
    }
}

#[derive(Debug, PartialEq, Clone)]
pub enum AudioSampleFormat {
    BitStream,
    Eight { unsigned: bool },
    Sixteen { unsigned: bool, invert_endian: bool },
    TwentyPacked { unsigned: bool },
    TwentyFourPacked { unsigned: bool },
    TwentyIn32 { unsigned: bool, invert_endian: bool },
    TwentyFourIn32 { unsigned: bool, invert_endian: bool },
    ThirtyTwo { unsigned: bool, invert_endian: bool },
    Float,
}

impl TryFrom<u32> for AudioSampleFormat {
    fn try_from(value: u32) -> Result<Self> {
        const UNSIGNED_FLAG: u32 = 1u32 << 30;
        const INVERT_ENDIAN_FLAG: u32 = 1u32 << 31;
        const FLAG_MASK: u32 = UNSIGNED_FLAG | INVERT_ENDIAN_FLAG;
        let unsigned = value & UNSIGNED_FLAG != 0;
        let invert_endian = value & INVERT_ENDIAN_FLAG != 0;
        let res = match value & !FLAG_MASK {
            0x0000_0001 => AudioSampleFormat::BitStream,
            0x0000_0002 => AudioSampleFormat::Eight { unsigned },
            0x0000_0004 => AudioSampleFormat::Sixteen { unsigned, invert_endian },
            0x0000_0010 => AudioSampleFormat::TwentyPacked { unsigned },
            0x0000_0020 => AudioSampleFormat::TwentyFourPacked { unsigned },
            0x0000_0040 => AudioSampleFormat::TwentyIn32 { unsigned, invert_endian },
            0x0000_0080 => AudioSampleFormat::TwentyFourIn32 { unsigned, invert_endian },
            0x0000_0100 => AudioSampleFormat::ThirtyTwo { unsigned, invert_endian },
            0x0000_0200 => AudioSampleFormat::Float,
            _ => return Err(Error::OutOfRange),
        };
        Ok(res)
    }
}

impl From<&AudioSampleFormat> for u32 {
    fn from(v: &AudioSampleFormat) -> u32 {
        const UNSIGNED_FLAG: u32 = 1u32 << 30;
        const INVERT_ENDIAN_FLAG: u32 = 1u32 << 31;
        match v {
            AudioSampleFormat::BitStream => 0x0000_0001,
            AudioSampleFormat::Eight { unsigned } => {
                let flag = if *unsigned { UNSIGNED_FLAG } else { 0 };
                flag | 0x0000_0002
            }
            AudioSampleFormat::Sixteen { unsigned, invert_endian } => {
                let mut flags = if *unsigned { UNSIGNED_FLAG } else { 0 };
                flags = if *invert_endian { INVERT_ENDIAN_FLAG | flags } else { flags };
                flags | 0x0000_0004
            }
            AudioSampleFormat::TwentyPacked { unsigned } => {
                let flags = if *unsigned { UNSIGNED_FLAG } else { 0 };
                flags | 0x0000_0010
            }
            AudioSampleFormat::TwentyFourPacked { unsigned } => {
                let flags = if *unsigned { UNSIGNED_FLAG } else { 0 };
                flags | 0x0000_0020
            }
            AudioSampleFormat::TwentyIn32 { unsigned, invert_endian } => {
                let mut flags = if *unsigned { UNSIGNED_FLAG } else { 0 };
                flags = if *invert_endian { INVERT_ENDIAN_FLAG | flags } else { flags };
                flags | 0x0000_0040
            }
            AudioSampleFormat::TwentyFourIn32 { unsigned, invert_endian } => {
                let mut flags = if *unsigned { UNSIGNED_FLAG } else { 0 };
                flags = if *invert_endian { INVERT_ENDIAN_FLAG | flags } else { flags };
                flags | 0x0000_0080
            }
            AudioSampleFormat::ThirtyTwo { unsigned, invert_endian } => {
                let mut flags = if *unsigned { UNSIGNED_FLAG } else { 0 };
                flags = if *invert_endian { INVERT_ENDIAN_FLAG | flags } else { flags };
                flags | 0x0000_0100
            }
            AudioSampleFormat::Float => 0x0000_0200,
        }
    }
}

impl AudioSampleFormat {
    /// Compute the size of an audio frame based on the sample format.
    /// Returns Err(OutOfRange) in the case where it cannot be computed
    /// (bad channel count, bad sample format)
    pub fn compute_frame_size(&self, channels: usize) -> Result<usize> {
        let bytes_per_channel = match self {
            AudioSampleFormat::Eight { .. } => 1,
            AudioSampleFormat::Sixteen { .. } => 2,
            AudioSampleFormat::TwentyFourPacked { .. } => 3,
            AudioSampleFormat::TwentyIn32 { .. }
            | AudioSampleFormat::TwentyFourIn32 { .. }
            | AudioSampleFormat::ThirtyTwo { .. }
            | AudioSampleFormat::Float => 4,
            _ => return Err(Error::OutOfRange),
        };
        Ok(channels * bytes_per_channel)
    }
}
