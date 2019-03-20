use super::{device_cap, operclass, regulation, utils};
/// Returns the jurisdiction of operation.
/// Never fails to return. The fallback is "GLOBAL".
use failure::{bail, Error};

#[allow(dead_code)]
#[derive(Deserialize)]
pub struct IsoAlpha2 {
    version: String,
    country_codes: Vec<String>,
}

#[derive(Deserialize, Debug)]
pub struct Jurisdictions {
    version: String,
    supported: Vec<String>,
}

pub fn load_supported_jurisdictions() -> Result<Vec<String>, Error> {
    const FILENAME: &str = "./data/jurisdictions.toml";
    let contents = utils::load_file(FILENAME)?;
    let jurisdictions: Jurisdictions = toml::from_str(contents.as_str())?;
    validate_jurisdictions(&jurisdictions)?;
    Ok(jurisdictions.supported)
}

pub fn validate_jurisdictions(jurisdictions: &Jurisdictions) -> Result<(), Error> {
    let iso_alpha2 = load_iso_alpha2()?;
    for j in &jurisdictions.supported {
        // By design, two letters must be ISO Alpha-2
        if j.len() == 2 {
            if !iso_alpha2.contains(j) {
                bail!("jurisdiction {} is of two-letters but not part of ISO Alpha-2", j);
            }
        }

        // Perform cross check.
        // A jurisdiction is valid if
        // (1) a corresponding and valid regulation file exists
        // (2) a corresponding and valid operating class file exists.
        // (3) a mock device XYZ has defined the list of active operating classes.

        let filepath_juris = operclass::get_filepath(j);
        if !operclass::load_operclasses(filepath_juris.as_str()).is_ok() {
            bail!("jurisdiction {} does not have a corresponding and valid OperClass file", j)
        }
        let filepath_reg = regulation::get_filepath(j);
        if !regulation::load_regulations(&filepath_reg).is_ok() {
            bail!("jurisdiction {} does not have a corresponding and valid Regulation file", j)
        }
        if !device_cap::get_operclasses(j).is_ok() {
            bail!(
                "active operating classes for jurisdiction {} is not defined in a mock device XYZ",
                j
            );
        }
    }
    Ok(())
}

pub fn which_country_am_i_in() -> String {
    return "US".to_string();
}

pub fn load_iso_alpha2() -> Result<Vec<String>, Error> {
    const FILENAME: &str = "./data/iso_alpha2.toml";
    let contents = utils::load_file(FILENAME)?;
    let iso_alpha2: IsoAlpha2 = toml::from_str(contents.as_str())?;

    validate_iso_alpha2(&iso_alpha2)?;

    Ok(iso_alpha2.country_codes)
}

fn validate_iso_alpha2(iso_alpha2: &IsoAlpha2) -> Result<(), Error> {
    for c in &iso_alpha2.country_codes {
        // Two bytes ASCII string in upper case.
        if c.len() != 2 {
            bail!("Country code '{}' is not 2 bytes long", c);
        }

        let is_ascii = c.to_ascii_uppercase() == *c;
        if !is_ascii {
            bail!("Country code '{}' is not ASCII uppercase", c);
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_supported_jurisdictions() {
        match load_supported_jurisdictions() {
            Err(e) => {
                println!("{}", e);
                assert!(false);
            }
            Ok(_) => (),
        };
    }
}
