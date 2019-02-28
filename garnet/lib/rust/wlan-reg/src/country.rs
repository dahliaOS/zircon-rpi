use super::utils;
/// Returns the jurisdiction of operation.
/// Never fails to return. The fallback is "GLOBAL".
use failure::Error;

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
    Ok(iso_alpha2.country_codes)
}
