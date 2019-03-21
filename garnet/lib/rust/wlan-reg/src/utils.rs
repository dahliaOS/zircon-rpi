// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::Error;
use std::{fs, fs::File, io::prelude::*, path::PathBuf};

pub fn load_file(filepath: &str) -> Result<String, Error> {
    let rel_path = PathBuf::from(filepath);
    let _abs_path = fs::canonicalize(&rel_path)?;
    let mut file = File::open(filepath)?;
    let mut contents = String::new();
    file.read_to_string(&mut contents)?;
    Ok(contents)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_file() {
        const FILENAME: &str = "./data/iso_alpha2.toml";
        let result = load_file(FILENAME);
        assert!(result.is_ok());
    }
}
