extern crate log;
extern crate serde_derive;
extern crate toml;

use super::utils;
use failure::{bail, Error};
use toml::Value;
use toml::Value::Table;

/// Take the file path for an Operating Class TOML file,
/// returns TOML Value if validated, otherwise, error.
pub fn load_toml(filepath: &str) -> Result<Value, Error> {
    let toml = utils::load_toml(filepath)?;
    validate(toml)
    //        Err(e) => {
    //            bail!("{}", e);
    //        }
    //        _ => (),
    //    };
    //    Ok(toml)
}

/// Passes the validation if mandatory fields are present.
/// Optional fields, or unidentifiable fields are don't care ones.
pub fn validate(v: Value) -> Result<Value, Error> {
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
pub fn get_filepath(jurisdiction: &str) -> String {
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
    fn test_get_filepath() {
        let got = get_filepath("XYZ");
        let want = "./data/regulation_XYZ.toml".to_string();
        assert_eq!(got, want);
    }

    #[test]
    fn test_load_toml() {
        const FILES: [&str; 2] = ["./data/regulation_US.toml", "./data/regulation_GLOBAL.toml"];
        for f in FILES.iter() {
            match load_toml(f) {
                Ok(_) => (),
                Err(e) => { println!("Error while processing {} : {}", f,e); assert!(false)},
            };
        }
    }


}
