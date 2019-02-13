extern crate log;
extern crate serde_derive;
extern crate toml;

use super::utils;
use failure::{bail, Error};
use log::info;
use toml::Value;
use toml::Value::Table;

/// Take the file path for an Operating Class TOML file,
/// returns TOML Value if validated, otherwise, error.
pub fn load_operating_class_toml(filepath: &str) -> Result<Value, Error> {
    let toml = utils::load_toml(filepath)?;
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

/// Returns the TOML file name containing the Operating Classes of the jurisdiction of the operation
pub fn get_operating_class_filename(jurisdiction: &str) -> String {
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
pub fn load_regulation_toml(filepath: &str) -> Result<Value, Error> {
    let toml = utils::load_toml(filepath)?;
    validate_regulation(toml)
    //        Err(e) => {
    //            bail!("{}", e);
    //        }
    //        _ => (),
    //    };
    //    Ok(toml)
}

/// Passes the validation if mandatory fields are present.
/// Optional fields, or unidentifiable fields are don't care ones.
pub fn validate_regulation(v: Value) -> Result<Value, Error> {
    const MANDATORY_FIELDS: &'static [&'static str] = &["version", "jurisdiction"];
    for f in MANDATORY_FIELDS.iter() {
        if v.get(f).is_none() {
            bail!("mandatory field missing: {}", f);
        };
    }
    let table = match &v {
        Table(t) => t,
        _ => {
            bail!("not a table! : {}", v);
        }
    };
    for (k, vv) in table.iter() {
        if let Table(_) = vv {
            if let Err(e) = validate_subband(&vv) {
                bail!("Key {} has error: {}", k, e);
            }
        }
    }
    Ok(v)
}

fn validate_subband(v: &Value) -> Result<(), Error> {
    if v.get("do_not_use").is_some() {
        return Ok(());
    }

    const MANDATORY_FIELDS1: &'static [&'static str] = &["freq_beg", "freq_end"];
    for f in MANDATORY_FIELDS1.iter() {
        if v.get(f).is_none() {
            bail!("mandatory field missing: {}", f);
        };
        if !v[f].is_float() {
            bail!("field {} is non-float: {} ", f, v[f]);
        }
    }
    let freq_beg = v["freq_beg"].as_float().unwrap() as f64;
    let freq_end = v["freq_end"].as_float().unwrap() as f64;
    if freq_beg > freq_end {
        bail!("freq_beg {} is greater than freq_end {}", freq_beg, freq_end);
    }

    const MANDATORY_FIELDS2: &'static [&'static str] = &["chan_idx_beg", "chan_idx_end"];
    for f in MANDATORY_FIELDS2.iter() {
        if v.get(f).is_none() {
            bail!("mandatory field missing: {}", f);
        };
        if !v[f].is_integer() {
            bail!("field {} is non-integer: {} ", f, v[f]);
        }
    }
    let chan_idx_beg = v["chan_idx_beg"].as_integer().unwrap() as u64;
    let chan_idx_end = v["chan_idx_end"].as_integer().unwrap() as u64;
    if chan_idx_beg > chan_idx_end {
        bail!("chan_idx_beg {} is greater than chan_idx_end {}", chan_idx_beg, chan_idx_end);
    }

    const CHOICE_FIELDS: &'static [&'static str] = &["role-any", "role-client"];
    let mut found_choice = false;
    for f in CHOICE_FIELDS.iter() {
        if v.get(f).is_some() {
            found_choice = true;
            break;
        };
    }
    if found_choice == false {
        bail!("choice field missing: {:?}", CHOICE_FIELDS);
    }

    validate_power_budget(v)
}

fn validate_power_budget(v: &Value) -> Result<(), Error> {
    let table = match v {
        Table(t) => t,
        _ => {
            bail!("not a table! : {}", v);
        }
    };

    const MANDATORY_FIELDS: &'static [&'static str] =
        &["role", "max_conduct_power", "max_ant_gain"];
    for (k, vv) in table.iter() {
        if let Table(_) = vv {
            if !k.starts_with("role-") {
                bail!("found a non-role specific table: {}", k);
            }
            for f in MANDATORY_FIELDS.iter() {
                if vv.get(f).is_none() {
                    bail!("table {} misses a mandatory field: {}", k, f);
                }
            }
        }
    }

    Ok(())
}

/// Returns the TOML file name containing the Regulations of the juristiction of the operation
pub fn get_regulation_filename(jurisdiction: &str) -> String {
    const REGULATION_FILENAME_DIR: &str = "./data/";
    const REGULATION_FILENAME_PREFIX: &str = "regulation_";
    const REGULATION_FILENAME_SUFFIX: &str = ".toml";
    format!(
        "{}{}{}{}",
        REGULATION_FILENAME_DIR,
        REGULATION_FILENAME_PREFIX,
        jurisdiction,
        REGULATION_FILENAME_SUFFIX,
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
    #[test]
    fn test_get_operating_class_filename() {
        let got = get_operating_class_filename("XYZ");
        let want = "./data/operating_class_XYZ.toml".to_string();
        assert_eq!(got, want);
    }
}
