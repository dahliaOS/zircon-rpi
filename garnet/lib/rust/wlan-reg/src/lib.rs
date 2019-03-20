// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[macro_use]
extern crate serde_derive;
extern crate toml;

pub mod channel;
pub mod country;
pub mod device_cap;
pub mod operclass;
pub mod power;
pub mod regulation;
pub mod sku;
pub mod utils;

#[macro_export]
macro_rules! vec_string {
    ($($x:expr),*) => (vec![$($x.to_string()),*]);
}
