use super::utils;
//use super::tablewrap;
use failure::{bail, Error};

extern crate toml;

#[allow(dead_code)]
#[derive(Deserialize, Debug)]
pub struct SkuTable {
    pub version: String,
    pub sku: Vec<SkuInfo>,
}

#[derive(Deserialize, Debug)]
pub struct SkuInfo {
    pub name: String,
    pub wlan_country_code: String,
    pub eligible_country: Vec<String>,
}

pub fn read_sku() -> String {
    "europe".to_string().to_lowercase()
}

pub fn get_sku_info(sku_name: String) -> Result<SkuInfo, Error> {
    const FILENAME: &str = "./data/sku_countries.toml";
    let contents = utils::load_file(FILENAME).unwrap();

    println!("{:#?}", contents);

    let sku_table: SkuTable = toml::from_str(contents.as_str())?;
    for elem in sku_table.sku {
        if sku_name == elem.name {
            return Ok(elem);
        }
    }

    bail!("SKU name {} was not found from the file {}", sku_name, FILENAME)
}
