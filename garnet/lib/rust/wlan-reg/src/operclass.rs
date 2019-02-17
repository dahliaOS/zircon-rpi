extern crate log;
extern crate serde_derive;
extern crate toml;

use super::utils;
use super::vec_string;
use failure::{bail, Error};
use log::info;
use toml::value::Table;
use toml::Value;

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
pub fn load_toml(filepath: &str) -> Result<Table, Error> {
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
fn validate(v: &Table) -> Result<(), Error> {
    let mandatory_fields = vec_string!["version", "jurisdiction"];
    for m in mandatory_fields.iter() {
        if !v.contains_key(m) {
            bail!("mandatory field missing: {}", m);
        };
    }

    const OPERCLASS_IDX_MIN: u8 = 1;
    const OPERCLASS_IDX_MAX: u8 = 255;
    for idx in OPERCLASS_IDX_MIN..=OPERCLASS_IDX_MAX {
        let mut key = format!("{}-{}", v["jurisdiction"], idx);
        key = str::replace(key.as_str(), "\"", "");
        if !v.contains_key(&key) {
            continue;
        }
        info!("Operating class {} found", &key);

        let operclass = match &v[&key] {
            Value::Table(t) => t,
            _ => continue,
        };

        if let Err(e) = validate_operclass(&operclass, idx) {
            bail!("Failed to validate operclass {}: {}", key, e);
        }
    }

    Ok(())
}

fn validate_operclass(t: &Table, operclass_idx: u8) -> Result<(), Error> {
    let mandatory_fields =
        vec_string!["oper_class", "start_freq", "spacing", "set", "center_freq_idx"];
    for m in mandatory_fields.iter() {
        if !t.contains_key(m) {
            bail!("mandatary field not found: {}", m);
        }
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
        const FILES: [&str; 2] =
            ["./data/operating_class_US.toml", "./data/operating_class_GLOBAL.toml"];
        for f in FILES.iter() {
            match load_toml(f) {
                Ok(_) => (),
                Err(e) => {
                    println!("Error while processing {} : {}", f, e);
                    assert!(false)
                }
            };
        }
    }

    #[test]
    fn test_get_filepath() {
        let got = get_filepath("XYZ");
        let want = "./data/operating_class_XYZ.toml".to_string();
        assert_eq!(got, want);
    }
}
