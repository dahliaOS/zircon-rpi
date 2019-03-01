//use super::utils;
use super::tablewrap;
use failure::Error;

#[allow(dead_code)]
#[derive(Deserialize, Debug)]
pub struct SkuTable {
    pub version: String,
    pub sku_vec: Vec<Sku>,
}

#[derive(Deserialize, Debug)]
pub struct Sku {
    pub wlan_country_code: String,
    pub countries: Vec<String>,
}

pub fn load_sku_table() -> Result<SkuTable, Error> {
    const FILENAME: &str = "./data/sku_countries.toml";
    let wrap = tablewrap::TableWrap::new(FILENAME);
    //    let contents = utils::load_file(FILENAME)?;
    //    let sku_table : SkuTable = toml::from_str(contents.as_str())?;

    //    validate_sku_table(sku_table)?;

    let sku_america = wrap.get_nested("sku_america");
    println!("{:?}", sku_america.get_vec_str("countries"));
    println!("{:?}", wrap.is_table_valid);

    let sku_table = SkuTable { version: "0.1.0".to_string(), sku_vec: vec![] };
    Ok(sku_table)
}
