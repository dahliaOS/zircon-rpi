// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Write virtio devices in rust :)

#![allow(warnings)]
#[macro_use] extern crate failure;

pub mod ring;
mod queue;

pub use crate::queue::*;

mod desc;

pub use desc::*;

pub mod util;
pub use util::*;
