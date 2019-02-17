extern crate toml;

use super::utils;
use failure::{bail, Error};
use toml::value::Table;
use toml::Value;

/// Take the file path for an Operating Class TOML file,
/// returns TOML Value if validated, otherwise, error.
pub fn load_toml(filepath: &str) -> Result<Table, Error> {
    let toml = utils::load_toml(filepath)?;
    match validate(&toml) {
        Ok(()) => Ok(toml),
        Err(e) => {
            bail!("{}", e);
        }
    }
}

pub fn validate(v: &Table) -> Result<(), Error> {
    const MANDATORY_FIELDS: &'static [&'static str] = &["version", "channels", "operating_classes"];
    for f in MANDATORY_FIELDS.iter() {
        if !v.contains_key(&f.to_string()) {
            bail!("mandatory field missing: {}", f);
        };
    }
    let table = match &v["operating_classes"] {
        Value::Table(t) => t,
        _ => {
            bail!("not a table! :");
        }
    };

    for (k, vv) in table.iter() {
        if !vv.is_array() {
            bail!("Field {} is not an array", k);
        }
        if vv.get(0).is_some() && !vv[0].is_integer() {
            bail!("Field 'set' is non-integer");
        };
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
    let toml = load_toml(filepath.as_str())?;
    Ok(utils::get_chanlist(&toml["channels"]))
}

pub fn get_operating_classes(juris: &str) -> Result<Vec<u8>, Error> {
    let filepath = get_filepath();
    let toml = load_toml(filepath.as_str())?;
    let ops = &toml["operating_classes"][juris];
    Ok(utils::get_chanlist(&ops))
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
    fn test_get_operating_classes() {
        let want: Vec<u8> =
            vec![1, 2, 3, 4, 5, 12, 22, 23, 24, 26, 27, 28, 29, 31, 32, 34, 128, 129, 130];
        if let Ok(got) = get_operating_classes("US") {
            assert_eq!(got, want);
        }
    }
}
