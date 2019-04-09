// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]
#![recursion_limit = "1024"]

pub use crate::types::{Error, Result};

/// Generic types
#[macro_use]
mod types;

/// Audio utilities
pub mod audio;

// Tools for encoding and decoding
//pub mod codec;
