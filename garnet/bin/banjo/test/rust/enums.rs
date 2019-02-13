// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.enums banjo file

use fuchsia_zircon as zircon;
use fuchsia_ddk_sys as ddk;


// C ABI compat

#[repr(i8)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum int8_enum_t {
    KNEGATIVEONE = -1,
    KONE = 1,
}

#[repr(i16)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum int16_enum_t {
    KNEGATIVEONE = -1,
    KONE = 1,
    KTWO = 2,
}

#[repr(i32)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum int32_enum_t {
    KNEGATIVEONE = -1,
    KONE = 1,
    KTWO = 2,
    KTHREE = 3,
}

#[repr(i64)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum int64_enum_t {
    KNEGATIVEONE = -1,
    KONE = 1,
    KTWO = 2,
    KTHREE = 3,
    KFOUR = 4,
}

#[repr(u8)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum uint8_enum_t {
    KONE = 1,
    KTWO = 2,
    KTHREE = 3,
    KFOUR = 4,
    KFIVE = 5,
}

#[repr(u16)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum uint16_enum_t {
    KONE = 1,
    KTWO = 2,
    KTHREE = 3,
    KFOUR = 4,
    KFIVE = 5,
    KSIX = 6,
}

#[repr(u32)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum uint32_enum_t {
    KONE = 1,
    KTWO = 2,
    KTHREE = 3,
    KFOUR = 4,
    KFIVE = 5,
    KSIX = 6,
    KSEVEN = 7,
}

#[repr(u64)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum uint64_enum_t {
    KONE = 1,
    KTWO = 2,
    KTHREE = 3,
    KFOUR = 4,
    KFIVE = 5,
    KSIX = 6,
    KSEVEN = 7,
    KEIGHT = 8,
}

// idiomatic bindings
