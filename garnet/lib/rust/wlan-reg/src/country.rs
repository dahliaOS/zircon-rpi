use super::utils;
/// Returns the jurisdiction of operation.
/// Never fails to return. The fallback is "GLOBAL".
use failure::{bail, Error};

#[allow(dead_code)]
#[derive(Deserialize)]
struct IsoAlpha2 {
    version: String,
    country_codes: Vec<String>,
}

pub fn get_jurisdiction() -> String {
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
