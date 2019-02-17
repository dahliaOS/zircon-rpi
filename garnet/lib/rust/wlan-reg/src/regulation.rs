extern crate log;
extern crate serde_derive;
extern crate toml;

use super::utils;
use super::vec_string;
use failure::{bail, Error};
use toml::value::Table;
use toml::Value;

/// Take the file path for an Operating Class TOML file,
/// returns TOML Value if validated, otherwise, error.
pub fn load_toml(filepath: &str) -> Result<Table, Error> {
    let toml = utils::load_toml(filepath)?;
    validate(toml)
}

/// Passes the validation if mandatory fields are present.
/// Optional fields, or unidentifiable fields are don't care ones.
pub fn validate(v: Table) -> Result<Table, Error> {
    const MANDATORY_FIELDS: &'static [&'static str] = &["version", "jurisdiction"];
    for f in MANDATORY_FIELDS.iter() {
        if !v.contains_key(&f.to_string()) {
            bail!("mandatory field missing: {}", f);
        };
    }
    for (k, vv) in v.iter() {
        let subband = match vv {
            Value::Table(t) => t,
            _ => continue,
        };
        if let Err(e) = validate_subband(subband) {
            bail!("Key {} has error: {}", k, e);
        }
    }
    Ok(v)
}

fn validate_subband(v: &Table) -> Result<(), Error> {
    if v.get("do_not_use").is_some() {
        return Ok(());
    }

    let mandatory_fields1 = vec_string!["freq_beg", "freq_end"];
    for m in mandatory_fields1.iter() {
        if !v.contains_key(m) {
            bail!("mandatory field missing: {}", m);
        };
        if !v[m].is_float() {
            bail!("field {} is non-float: {} ", m, v[m]);
        }
    }
    let freq_beg = v["freq_beg"].as_float().unwrap() as f64;
    let freq_end = v["freq_end"].as_float().unwrap() as f64;
    if freq_beg > freq_end {
        bail!("freq_beg {} is greater than freq_end {}", freq_beg, freq_end);
    }

    let mandatory_fields2 = vec_string!["chan_idx_beg", "chan_idx_end"];
    for m in mandatory_fields2.iter() {
        if !v.contains_key(m) {
            bail!("mandatory field missing: {}", m);
        };
        if !v[m].is_integer() {
            bail!("field {} is non-integer: {} ", m, v[m]);
        }
    }
    let chan_idx_beg = v["chan_idx_beg"].as_integer().unwrap() as u64;
    let chan_idx_end = v["chan_idx_end"].as_integer().unwrap() as u64;
    if chan_idx_beg > chan_idx_end {
        bail!("chan_idx_beg {} is greater than chan_idx_end {}", chan_idx_beg, chan_idx_end);
    }

    let choice_fields = vec_string!["role-any", "role-client"];
    let mut found_choice = false;
    for f in choice_fields.iter() {
        if v.contains_key(f) {
            found_choice = true;
            break;
        };
    }
    if found_choice == false {
        bail!("choice field missing: {:?}", choice_fields);
    }

    validate_power_budget(v)
}

fn validate_power_budget(v: &Table) -> Result<(), Error> {
    const MANDATORY_FIELDS: &'static [&'static str] =
        &["role", "max_conduct_power", "max_ant_gain"];
    for (k, vv) in v.iter() {
        if let Value::Table(_) = vv {
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
                Err(e) => {
                    println!("Error while processing {} : {}", f, e);
                    assert!(false)
                }
            };
        }
    }
}
