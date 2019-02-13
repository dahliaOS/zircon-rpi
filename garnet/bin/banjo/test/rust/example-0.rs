// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.example0 banjo file

use fuchsia_zircon as zircon;
use fuchsia_ddk_sys as ddk;

// C ABI compat

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct foo_t {
    pub b: bar_t,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct bar_t {
    pub f: *mut foo_t,
}

// idiomatic bindings
