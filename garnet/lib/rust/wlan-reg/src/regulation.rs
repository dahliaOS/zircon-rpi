extern crate log;
extern crate serde_derive;
extern crate toml;

use super::utils;
use failure::{bail, Error};

#[allow(dead_code)]
#[derive(Deserialize, Debug)]
pub struct RegulationTable {
    pub version: String, // ISO alpha-2
    pub jurisdiction: String,
    pub subband: Vec<Subband>,
}

#[derive(Deserialize, Debug)]
pub struct Subband {
    pub freq_beg: f64,    // GHz
    pub freq_end: f64,    // GHz
    pub chan_idx_beg: u8, // unitless IEEE index
    pub chan_idx_end: u8, // unitless IEEE index

    pub is_indoor: bool,  // at least one role is legal to use indoor, if true.
    pub is_outdoor: bool, // at least one role is legal to use outdoor, if true.

    pub role: Vec<Role>,
    pub dfs: Option<Dfs>,
}

#[derive(Deserialize, Debug)]
pub struct Role {
    pub role: String,
    pub max_conduct_power: i8, // dBm
    pub max_ant_gain: u8,      // dBi
}

#[derive(Deserialize, Debug, Clone)]
pub struct Dfs {
    pub is_dfs: bool,
    pub dfs_detect_thresh: i8,                     // dBm
    pub dfs_detect_thresh_for_lower_eirp: i8,      // dBm
    pub dfs_channel_availability_check_time: u32,  // msec
    pub dfs_channel_move_time: u32,                // msec
    pub dfs_channel_closing_tx_time: u32,          // msec
    pub dfs_channel_closing_tx_time_leftover: u32, // msec
    pub dfs_non_occupancy_period: u32,             // msec
}

pub fn load_regulations(filepath: &str) -> Result<RegulationTable, Error> {
    let contents = match utils::load_file(filepath) {
        Err(e) => {
            bail!("{} in reading {}", e, filepath);
        }
        Ok(c) => c,
    };
    let reg_table: RegulationTable = toml::from_str(contents.as_str())?;

    // println!("{:#?}", reg_table);
    validate_regulations(&reg_table)?;
    Ok(reg_table)
}

pub fn validate_regulations(reg_table: &RegulationTable) -> Result<(), Error> {
    // jurisdiction should be ISO alpha-2

    for s in &reg_table.subband {
        let name = format!("subband [{}, {}]", s.freq_beg, s.freq_end);
        // freq_beg > freq_end
        if s.freq_beg >= s.freq_end {
            bail!("{} frequency range is invalid", name);
        }

        // chan_idx_beg > chan_idx_end
        if s.chan_idx_beg > s.chan_idx_end {
            bail!(
                "{} channel index range is invalid: [{}, {}]",
                name,
                s.chan_idx_beg,
                s.chan_idx_end
            );
        }

        // at least one should be true: is_indoor, is_outdoor
        if !(s.is_indoor || s.is_outdoor) {
            bail!("{} supports none of indoor and outdoor", name)
        }

        // role should be one of "any", "indoor_ap", "outdoor_ap", "fixed_p2p_ap", "client"
        let valid_roles =
            vec!["any", "indoor_ap", "outdoor_ap", "fixed_p2p_ap", "fixed_p2p", "client"];
        for r in &s.role {
            if !valid_roles.contains(&r.role.as_str()) {
                bail!("{} has invalid role: {}", name, r.role);
            }
        }

        if s.dfs.is_some() {
            let dfs = s.dfs.clone().unwrap();
            // dfs_detect_thresh is expected to be negative
            if dfs.dfs_detect_thresh >= 0 {
                bail!("{} has non-negative dfs_detect_thresh {}", name, dfs.dfs_detect_thresh);
            }
            // dfs_detect_thresh_for_lower_eirp is expected to be negative
            if dfs.dfs_detect_thresh_for_lower_eirp >= 0 {
                bail!(
                    "{} has non-negative dfs_detect_thresh_for_lower_eirp {}",
                    name,
                    dfs.dfs_detect_thresh_for_lower_eirp
                );
            }
            // dfs_channel_availability_check_time > dfs_channel_move_time
            if dfs.dfs_channel_availability_check_time < dfs.dfs_channel_move_time {
                bail!(
                    "{} dfs_channel_availability_check_time is smaller than dfs_channel_move_time",
                    name
                );
            }
            // dfs_channel_move_time > dfs_channel_closing_tx_time + dfs_channel_closing_tx_time_leftover
            if dfs.dfs_channel_move_time
                < dfs.dfs_channel_closing_tx_time + dfs.dfs_channel_closing_tx_time_leftover
            {
                bail!(
                    "{} dfs_channel_move_time, closing_tx_time, closing_tx_time_leftover mismatch",
                    name
                );
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
    fn test_load() {
        const FILES: [&str; 2] = ["./data/regulation_US.toml", "./data/regulation_GLOBAL.toml"];
        for f in FILES.iter() {
            match load_regulations(f) {
                Ok(_) => (),
                Err(e) => {
                    println!("Error while processing {} : {}", f, e);
                    assert!(false)
                }
            };
        }
    }
}
