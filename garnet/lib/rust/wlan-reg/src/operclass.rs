extern crate log;
extern crate serde_derive;
extern crate toml;

use super::utils;
//use super::vec_string;
use failure::{bail, Error};
//use toml::value::Table;
//use toml::Value;

#[derive(Deserialize, Debug)]
pub struct OperClassTable {
    pub version: String,
    pub jurisdiction: String, // ISO alpha-2
    pub operclass: Vec<OperClass>,
}

#[derive(Deserialize, Debug)]
pub struct OperClass {
    pub idx: u8,         // [1, 255]
    pub start_freq: f64, // GHz
    pub spacing: f64,    // MHz
    pub set: Vec<u8>,
    pub center_freq_idx: Vec<u8>,

    pub nomadic: bool,
    pub license_exempt: bool,
    pub primary_chan_lower: bool,
    pub primary_chan_upper: bool,
    pub cca_de: bool,
    pub dfs_50_100: bool,
    pub its_nonmobile: bool,
    pub cbw80plus: bool,
    pub use_eirp_vht_txpower_env: bool,
    pub geo_db: bool,
}

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

pub fn load_operclasses(filepath: &str) -> Result<OperClassTable, Error> {
    let contents = match utils::load_file(filepath) {
        Err(e) => {
            bail!("{} in reading {}", e, filepath);
        }
        Ok(c) => c,
    };
    let operclass_table: OperClassTable = toml::from_str(contents.as_str())?;

    //    println!("OperclassTable\n{:#?}", operclass_table);
    validate_operclasses(&operclass_table)?;
    Ok(operclass_table)
}

pub fn validate_operclasses(operclass_table: &OperClassTable) -> Result<(), Error> {
    // TODO(porce): Validate version
    // TODO(porce): Validate the jurisdiction

    if operclass_table.operclass.len() == 0 {
        bail!(
            "given operclass table for jurisdiction {} has no operclass definition",
            operclass_table.jurisdiction
        );
    }

    for o in &operclass_table.operclass {
        if o.idx < 1 {
            bail!(
                "Jurisdiction {} has an operclass with invalid operclass index: {}",
                operclass_table.jurisdiction,
                o.idx
            );
        }

        if o.start_freq < 2.4 || o.start_freq > 60.0 {
            bail!(
                "Jurisdiction {} has an operclass with invalid start freq: {}",
                operclass_table.jurisdiction,
                o.start_freq
            );
        }

        for chanidx in &o.set {
            if chanidx == &0 {
                bail!(
                    "Jurisdiction {} operclass idx {} has an invalid chan index: {}",
                    operclass_table.jurisdiction,
                    o.idx,
                    chanidx
                );
            }
        }

        if o.set.len() == 0 && o.center_freq_idx.len() == 0 {
            bail!(
                "Jurisdiction {} operclass idx {} has both empty set and center_freq_idx",
                operclass_table.jurisdiction,
                o.idx
            );
        }

        // One item: Relation between start_freq, spacing, and sets.
        // The spacing and channel indexing rules depend on the band (identifiable by start_freq) and the jurisdiction.
        // TODO(porce): Study if there is a clearer, unified rule.
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_operclasses() {
        const FILES: [&str; 2] =
            ["./data/operating_class_US.toml", "./data/operating_class_GLOBAL.toml"];
        for f in FILES.iter() {
            match load_operclasses(f) {
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
