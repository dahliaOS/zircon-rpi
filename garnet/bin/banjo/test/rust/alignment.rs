// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.alignment banjo file

#![allow(unused_imports)]
use fuchsia_zircon as zircon;
use fuchsia_ddk as ddk;

// C ABI compat

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct packing_0_t {
    pub i16_0: i16,
    pub i32_0: i32,
    pub i16_1: i16,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct packing_1_t {
    pub i16_0: i16,
    pub i8_0: i8,
    pub i16_1: i16,
    pub i8_1: i8,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct packing_2_t {
    pub i16_0: i16,
    pub i8_0: i8,
    pub i8_1: i8,
    pub i16_1: i16,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct packing_3_t {
    pub i32_0: i32,
    pub i64_0: i64,
    pub i16_0: i16,
    pub i32_1: i32,
    pub i16_1: i16,
}

// idiomatic bindings
