// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};
use fuchsia_component::client::connect_to_service;
use fuchsia_zircon::Status;
use parking_lot::{RwLock, RwLockUpgradableReadGuard};
use serde_json::Value;
use std::fs::read_dir;
use std::path::{Path, PathBuf};

pub mod macros {
    pub use crate::fx_err_and_bail;
    pub use crate::parse_arg;
    pub use crate::with_line;
}

#[macro_export]
macro_rules! parse_arg {
    ($args:ident, $func:ident, $name:expr) => {
        match $args.get($name) {
            Some(v) => match v.$func() {
                Some(val) => Ok(val),
                None => Err($crate::common_utils::error::Sl4fError::new(
                    format!("malformed {}", $name).as_str(),
                )),
            },
            None => Err($crate::common_utils::error::Sl4fError::new(
                format!("{} missing", $name).as_str(),
            )),
        }
    };
}

#[macro_export]
macro_rules! with_line {
    ($tag:expr) => {
        format!("{}:{}", $tag, line!())
    };
}

#[macro_export]
macro_rules! fx_err_and_bail {
    ($tag:expr, $msg:expr) => {{
        fx_log_err!(tag: $tag, "{}", $msg);
        return Err(format_err!($msg));
    }};
}

pub fn parse_identifier(args_raw: Value) -> Result<String, Error> {
    let id_raw = match args_raw.get("identifier") {
        Some(id) => id,
        None => return Err(format_err!("Connect peripheral identifier missing")),
    };

    let id = id_raw.as_str().map(String::from);

    match id {
        Some(id) => Ok(id),
        None => return Err(format_err!("Identifier missing")),
    }
}

pub fn parse_service_identifier(args_raw: Value) -> Result<u64, Error> {
    parse_arg!(args_raw, as_u64, "service_identifier").map_err(Into::into)
}

pub fn parse_u64_identifier(args_raw: Value) -> Result<u64, Error> {
    parse_arg!(args_raw, as_u64, "identifier").map_err(Into::into)
}

pub fn parse_offset(args_raw: Value) -> Result<u64, Error> {
    parse_arg!(args_raw, as_u64, "offset").map_err(Into::into)
}

pub fn parse_max_bytes(args_raw: Value) -> Result<u64, Error> {
    parse_arg!(args_raw, as_u64, "max_bytes").map_err(Into::into)
}

pub fn parse_psm(args_raw: Value) -> Result<u64, Error> {
    parse_arg!(args_raw, as_u64, "psm").map_err(Into::into)
}

pub fn parse_write_value(args_raw: Value) -> Result<Vec<u8>, Error> {
    let arr = parse_arg!(args_raw, as_array, "write_value")?;
    let mut vector: Vec<u8> = Vec::new();
    for value in arr.into_iter() {
        match value.as_u64() {
            Some(num) => vector.push(num as u8),
            None => {}
        };
    }
    Ok(vector)
}

/// Given a RwLock of an optional FIDL service proxy, returns a cached connection to the service,
/// or try to connect and cache the connection for later.
pub fn get_proxy_or_connect<S>(lock: &RwLock<Option<S::Proxy>>) -> Result<S::Proxy, Error>
where
    S: fidl::endpoints::DiscoverableService,
    S::Proxy: Clone,
{
    let lock = lock.upgradable_read();
    if let Some(proxy) = lock.as_ref() {
        Ok(proxy.clone())
    } else {
        let proxy = connect_to_service::<S>()?;
        *RwLockUpgradableReadGuard::upgrade(lock) = Some(proxy.clone());
        Ok(proxy)
    }
}

pub fn find_file(dir: &Path, pattern: &str) -> Result<PathBuf, Status> {
    for entry in read_dir(dir)? {
        let path = entry?.path();
        if path.ends_with(pattern) {
            return Ok(path);
        }

        if let Ok(res) = find_file(&path, pattern) {
            return Ok(res);
        }
    }
    Err(Status::INTERNAL)
}
