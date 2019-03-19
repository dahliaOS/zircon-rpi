extern crate toml;

use super::utils;
use failure::{bail, Error};

#[derive(Deserialize, Debug)]
pub struct DeviceCaps {
    pub version: String,
    pub channels: Vec<u8>,
    pub juris: Vec<JurisdictionCap>,
}

#[derive(Deserialize, Debug)]
pub struct JurisdictionCap {
    pub name: String,
    pub operclass: Vec<u8>,
}

pub fn load_device_caps(filepath: &str) -> Result<DeviceCaps, Error> {
    let contents = match utils::load_file(filepath) {
        Err(e) => {
            bail!("{} in reading {}", e, filepath);
        }
        Ok(c) => c,
    };
    let device_caps: DeviceCaps = toml::from_str(contents.as_str())?;

    // println!("DeviceCaps\n{:#?}", device_caps);
    validate_device_caps(&device_caps)?;
    Ok(device_caps)
}

pub fn validate_device_caps(device_caps: &DeviceCaps) -> Result<(), Error> {
    // TODO(porce): Validate version

    if device_caps.juris.len() == 0 {
        bail!("given device_caps table has no jurisdictions defined");
    }

    for j in &device_caps.juris {
        // TODO(porce): Validate the name
        if j.operclass.len() == 0 {
            bail!("given device_caps table's jurisdiction {} has empty operclass", j.name);
        }
    }

    Ok(())
}

// TODO(porce): Support other than typical WLAN devices
pub fn get_filepath() -> String {
    const FILENAME_DIR: &str = "./data/";
    const FILENAME_PREFIX: &str = "device_capability_";
    const FILENAME_SUFFIX: &str = ".toml";
    format!("{}{}{}{}", FILENAME_DIR, FILENAME_PREFIX, "XYZ", FILENAME_SUFFIX,)
}

pub fn get_channels() -> Result<Vec<u8>, Error> {
    let filepath = get_filepath();
    let device_caps = load_device_caps(filepath.as_str())?;
    Ok(device_caps.channels.clone())
}

/// Returns the operclasses the underlying device can utilize in that jurisdiction.
/// Note, the operclasses vary greatly from jurisdiction to jurisdiction.
pub fn get_operclasses(juris: &str) -> Result<Vec<u8>, Error> {
    let filepath = get_filepath();

    let device_caps = load_device_caps(filepath.as_str())?;
    for j in &device_caps.juris {
        if j.name != juris {
            continue;
        }
        return Ok(j.operclass.clone());
    }

    bail!("the device capability does not support the requested jurisdiction: {}", juris)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_get_channels() {
        let want: Vec<u8> = vec![
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112,
            116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165,
        ];
        if let Ok(got) = get_channels() {
            assert_eq!(got, want);
        }
    }

    #[test]
    fn test_get_operclasses() {
        let want: Vec<u8> =
            vec![1, 2, 3, 4, 5, 12, 22, 23, 24, 26, 27, 28, 29, 31, 32, 34, 128, 129, 130];
        if let Ok(got) = get_operclasses("US") {
            assert_eq!(got, want);
        }
    }
}
