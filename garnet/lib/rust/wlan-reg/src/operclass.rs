extern crate log;
extern crate serde_derive;
extern crate toml;

use super::utils;
use failure::{bail, Error};
use log::info;
use toml::Value;
use toml::Value::Table;

/// Returns the TOML file name containing the Operating Classes of the jurisdiction of the operation
pub fn get_filepath(jurisdiction: &str) -> String {
    const OPERATING_CLASS_FILENAME_DIR: &str = "./data/";
    const OPERATING_CLASS_FILENAME_PREFIX: &str = "operating_class_";
    const OPERATING_CLASS_FILENAME_SUFFIX: &str = ".toml";
    format!(
        "{}{}{}{}",
        OPERATING_CLASS_FILENAME_DIR,
        OPERATING_CLASS_FILENAME_PREFIX,
        jurisdiction,
        OPERATING_CLASS_FILENAME_SUFFIX
    )
}

/// Take the file path for an Operating Class TOML file,
/// returns TOML Value if validated, otherwise, error.
pub fn load_toml(filepath: &str) -> Result<Value, Error> {
    let toml = utils::load_toml(filepath)?;
    match validate(&toml) {
        Ok(()) => Ok(toml),
        Err(e) => {
            bail!("{}", e);
        }
    }
}

/// Passes the validation if mandatory fields are present.
/// Optional fields, or unidentifiable fields are don't care fields.
fn validate(v: &Value) -> Result<(), Error> {
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

    if !t["set"].is_array() {
        bail!("Field 'set' is not an array");
    } else {
        if t["set"].get(0).is_some() && !t["set"][0].is_integer() {
            bail!("Field 'set' is non-integer");
        }
    }

    if !t["center_freq_idx"].is_array() {
        bail!("Field 'center_freq_idx' has invalid value");
    } else {
        if t["center_freq_idx"].get(0).is_some() && !t["center_freq_idx"][0].is_integer() {
            bail!("Field 'center_freq_idx' is non-integer");
        }
    }

    // Cross-fields validation
    // Not universal test. Some operating classes miss set.
    if t["set"].get(0).is_none() && t["center_freq_idx"].get(0).is_none() {
        bail!("Both fields 'set' and 'center_freq_idx' are empty");
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_toml() {
        const FILE_PATH: &str = "./data/operating_class_US.toml";
        assert!(load_toml(FILE_PATH).is_ok());
    }
    #[test]
    fn test_get_filepath() {
        let got = get_filepath("XYZ");
        let want = "./data/operating_class_XYZ.toml".to_string();
        assert_eq!(got, want);
    }
}
