extern crate log;
extern crate serde_derive;
extern crate toml;

use failure::{bail, Error};
use log::{error, info};
use std::{
    fs,
    fs::File,
    io::prelude::*,
    //path::Path,
    path::PathBuf,
};
use toml::Value;
use toml::Value::Table;

/// Take the file path for an Operating Class TOML file,
/// returns TOML Value if validated, otherwise, error.
pub fn load_operating_class_toml(filepath: &String) -> Result<Value, Error> {
    // TODO(porce): Is this conversion needed? Why do we want to test abs path?
    let rel_path = PathBuf::from(filepath);
    let abs_path = fs::canonicalize(&rel_path);
    if let Err(e) = abs_path {
        bail!("Cannot get abs path for: {} : {} ", filepath, e);
    }

    let mut file = File::open(filepath)?;
    let mut contents = String::new();
    file.read_to_string(&mut contents)?;

    let toml = match contents.parse::<Value>() {
        Ok(t) => t,
        _ => {
            bail!("Failed to parse Operating Class TOML file: {}", filepath);
        }
    };

    match validate_operclasses(&toml) {
        Ok(()) => Ok(toml),
        Err(e) => {
            bail!("{}", e);
        }
    }
}

/// Passes the validation if mandatory fields are present.
/// Optional fields, or unidentifiable fields are don't care fields.
fn validate_operclasses(v: &Value) -> Result<(), Error> {
    const MANDATORY_FIELDS: &'static [&'static str] = &["version", "jurisdiction"];
    for f in MANDATORY_FIELDS.iter() {
        if v.get(f).is_none() {
            bail!("mandatory field missing: {}", f);
        };
    }

    const OPERCLASS_IDX_MIN: u8 = 1;
    const OPERCLASS_IDX_MAX: u8 = 255;
    for idx in OPERCLASS_IDX_MIN..=OPERCLASS_IDX_MAX {
        let mut key = format!("{}-{}", v["jurisdiction"], idx);
        key = str::replace(key.as_str(), "\"", "");
        if v.get(&key).is_none() {
            continue;
        }
        info!("Operating class {} found", &key);

        if let Err(e) = validate_operclass(&v[&key], idx) {
            bail!("Failed to validate operclass {}: {}", key, e);
        }
    }

    Ok(())
}

fn validate_operclass(v: &Value, operclass_idx: u8) -> Result<(), Error> {
    let t = match v {
        Table(t) => t,
        _ => {
            bail!("passed Value is not a Table");
        }
    };

    const MANDATORY_FIELDS: &'static [&'static str] =
        &["oper_class", "start_freq", "spacing", "set", "center_freq_idx"];

    for f in MANDATORY_FIELDS.iter() {
        if t.get(&f.to_string()).is_none() {
            bail!("mandatary field not found: {}", f);
        };
    }

    if !t["oper_class"].is_integer() {
        bail!("Field 'oper_class' is non-integer");
    }

    {
        let want = operclass_idx;
        let got = t["oper_class"].as_integer().unwrap() as u8;
        if want != got {
            bail!("Field 'oper_class' has a mismatching value. Expected {} Stored {}", want, got);
        }
    }

    if !t["start_freq"].is_float() {
        bail!("Field 'start_freq' has invalid value");
    }
    if !t["spacing"].is_integer() {
        bail!("Field 'spacing' has invalid value");
    }
    {
        if !t["set"].is_array() {
            bail!("Field 'set' is not an array");
        }

        // Not universal test. Some operating classes miss set.
        if t["set"].get(0).is_none() {
            bail!("Field 'set' is empty");
        }

        // Not universal test. Some operating classes miss set.
        // Testing the first index value only is sufficient.
        if !t["set"][0].is_integer() {
            bail!("Field 'set' is non-integer")
        }
    }

    if !t["center_freq_idx"].is_array() {
        bail!("Field 'center_freq_idx' has invalid value");
    }

    Ok(())
}

/// Returns the TOML file name containing the Operating Classes of the jurisdiction of the operation
pub fn get_operating_class_filename(jurisdiction: &String) -> String {
    const OPERATING_FLASS_FILENAME_DIR: &str = "./data/";
    const OPERATING_CLASS_FILENAME_PREFIX: &str = "operating_class_";
    const OPERATING_CLASS_FILENAME_SUFFIX: &str = ".toml";
    format!(
        "{}{}{}{}",
        OPERATING_FLASS_FILENAME_DIR,
        OPERATING_CLASS_FILENAME_PREFIX,
        jurisdiction,
        OPERATING_CLASS_FILENAME_SUFFIX
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_operating_class_toml() {
        const FILE_PATH: &str = "./data/operating_class_US.toml";
        assert!(load_operating_class_toml(FILE_PATH).is_ok());
    }
}
