extern crate toml;

use super::country;
use super::device_cap;
use super::operclass;
//use super::utils;

use failure::Error;
use std::fmt;
//use toml::value::Table;

#[derive(Debug, PartialOrd, PartialEq)]
pub struct ChannelGroups {
    pub band_2ghz: Vec<u8>,
    pub band_5ghz: Vec<u8>,
    pub dfs: Vec<u8>,
    pub cbw40above: Vec<u8>,
    pub cbw40below: Vec<u8>,
    pub cbw80center: Vec<u8>,
    pub cbw160center: Vec<u8>,

    // `all` is the union of above vectors.
    // Do not use `all` for scan/join purpose.
    pub all: Vec<u8>,
}

impl fmt::Display for ChannelGroups {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{:20}: {:?}\n{:20}: {:?}\n{:20}: {:?}\n{:20}: {:?}\n{:20}: {:?}\n{:20}: {:?}\n{:20}: {:?}\n{:20}: {:?}",
            "2 GHz", self.band_2ghz,
            "5 GHz", self.band_5ghz,
            "DFS", self.dfs,
            "CBW40 Above", self.cbw40above,
            "CBW40 Below", self.cbw40below,
            "CBW80 Center", self.cbw80center,
            "CBW160 Center", self.cbw160center,
            "All", self.all,
        )
    }
}

/// A Legitimate Channel Groups is a set of channel lists
/// defined in the jurisdiction of the device operation.
/// See also A Operation Channel Groups for comparison.
pub fn build_legitimate_group(
    table: &operclass::OperClassTable,
    active_operclasses: &Vec<u8>,
) -> ChannelGroups {
    let mut dfs = vec![];
    let mut band_2ghz = vec![];
    let mut band_5ghz = vec![];
    let mut cbw40above = vec![];
    let mut cbw40below = vec![];
    let mut cbw80center = vec![];
    let mut cbw160center = vec![];

    const START_FREQ_5GHZ_BAND: f64 = 5.000;
    const START_FREQ_2GHZ_BAND: f64 = 2.407;
    const SPACING_80MHZ: f64 = 80.0;
    const SPACING_160MHZ: f64 = 160.0;

    for o in &table.operclass {
        if !active_operclasses.contains(&o.idx) {
            continue;
        }

        if o.start_freq == START_FREQ_5GHZ_BAND {
            band_5ghz.extend(&o.set);
        }

        if o.start_freq == START_FREQ_2GHZ_BAND {
            band_2ghz.extend(&o.set);
        }

        if o.dfs_50_100 {
            dfs.extend(&o.set);
        }

        if o.primary_chan_lower {
            cbw40above.extend(&o.set);
        }

        if o.primary_chan_upper {
            cbw40below.extend(&o.set);
        }

        if o.spacing == SPACING_80MHZ {
            cbw80center.extend(&o.center_freq_idx);
        }

        if o.spacing == SPACING_160MHZ {
            cbw160center.extend(&o.center_freq_idx);
        }
    }

    band_2ghz.sort();
    band_2ghz.dedup();

    band_5ghz.sort();
    band_5ghz.dedup();

    dfs.sort();
    dfs.dedup();

    cbw40above.sort();
    cbw40above.dedup();

    cbw40below.sort();
    cbw40below.dedup();

    cbw80center.sort();
    cbw80center.dedup();

    cbw160center.sort();
    cbw160center.dedup();

    let mut all: Vec<u8> = [
        band_2ghz.as_slice(),
        band_5ghz.as_slice(),
        dfs.as_slice(),
        cbw40above.as_slice(),
        cbw40below.as_slice(),
        cbw80center.as_slice(),
        cbw160center.as_slice(),
    ]
    .concat();

    all.sort();
    all.dedup();

    ChannelGroups {
        band_2ghz,
        band_5ghz,
        dfs,
        cbw40above,
        cbw40below,
        cbw80center,
        cbw160center,
        all,
    }
}

/// An Operation Channel Group is a channel group that a device may operate with.
/// Conceptually, it is an intersection of legitimate channels and device-capable channels,
/// excluding Planned Non-Operation (PNO) channels and dynamically blocked channels
/// (eg. blocked due to radar / incombent higher priority radio transmitter presence).
pub fn build_operation_group(
    legit: ChannelGroups,
    capable: Vec<u8>,
    pno: Vec<u8>,
    blocked: Vec<u8>,
) -> ChannelGroups {
    let mut sieve = capable.clone();
    sieve.retain(|x| !pno.contains(x));
    sieve.retain(|x| !blocked.contains(x));

    let mut band_2ghz = legit.band_2ghz.clone();
    band_2ghz.retain(|x| sieve.contains(x));
    let mut band_5ghz = legit.band_5ghz.clone();
    band_5ghz.retain(|x| sieve.contains(x));
    let mut dfs = legit.dfs.clone();
    dfs.retain(|x| sieve.contains(x));
    let mut cbw40above: Vec<u8> = vec![];
    let mut cbw40below: Vec<u8> = vec![];
    let mut cbw80center: Vec<u8> = vec![];
    let mut cbw160center: Vec<u8> = vec![];

    // Test for cbw40above:
    // Both primary20 and secondary20 channels should be present in the channel lists.
    for c in legit.cbw40above.iter() {
        if band_2ghz.contains(&c) && band_2ghz.contains(&(c + 4)) {
            cbw40above.push(*c);
        }
        if band_5ghz.contains(&c) && band_5ghz.contains(&(c + 4)) {
            cbw40above.push(*c);
        }
    }

    // Test for cbw40below:
    // Both primary20 and secondary20 channels should be present in the channel lists.
    for c in legit.cbw40below.iter() {
        if band_2ghz.contains(&c) && band_2ghz.contains(&(c - 4)) {
            cbw40below.push(*c);
        }
        if band_5ghz.contains(&c) && band_5ghz.contains(&(c - 4)) {
            cbw40below.push(*c);
        }
    }

    // Test for cbw80center
    // all possible primary20 should be usable.
    for c in legit.cbw80center.iter() {
        let span: [u8; 4] = [c - 6, c - 2, c + 2, c + 6];
        if span.iter().all(|x| band_5ghz.contains(&x)) {
            cbw80center.push(*c);
        }
    }

    // Test for cbw160center
    // all possible primary20 should be usable.
    for c in legit.cbw160center.iter() {
        let span: [u8; 8] = [c - 14, c - 10, c - 6, c - 2, c + 2, c + 6, c + 10, c + 14];
        if span.iter().all(|x| band_5ghz.contains(&x)) {
            cbw160center.push(*c);
        }
    }

    let mut all: Vec<u8> = [
        band_2ghz.as_slice(),
        band_5ghz.as_slice(),
        dfs.as_slice(),
        cbw40above.as_slice(),
        cbw40below.as_slice(),
        cbw80center.as_slice(),
        cbw160center.as_slice(),
    ]
    .concat();

    all.sort();
    all.dedup();

    ChannelGroups {
        band_2ghz,
        band_5ghz,
        dfs,
        cbw40above,
        cbw40below,
        cbw80center,
        cbw160center,
        all,
    }
}

/// Returns what user configured not to use
pub fn get_planned_non_operation_chanidx_list() -> Vec<u8> {
    // Stub
    vec![2, 3, 4, 5, 7, 8, 9, 10, 144]
}

/// Returns a list of channel indexes that are dynamically blocked to use
/// due to radio environments - eg. radar
pub fn get_blocked_chanidx_list() -> Vec<u8> {
    // Stub
    vec![100]
}

pub fn get_legitimate_group() -> Result<ChannelGroups, Error> {
    let juris = country::which_country_am_i_in();
    let operclass_filepath = operclass::get_filepath(&juris);
    let operclass_table = operclass::load_operclasses(&operclass_filepath)?;
    let active_oper_class_indices = device_cap::get_operclasses(juris.as_str())?;
    Ok(build_legitimate_group(&operclass_table, &active_oper_class_indices))
}

pub fn get_operation_group() -> Result<ChannelGroups, Error> {
    let capable = device_cap::get_channels()?;
    let pno = get_planned_non_operation_chanidx_list();
    let blocked = get_blocked_chanidx_list();
    let legit_chan_groups = get_legitimate_group()?;
    Ok(build_operation_group(legit_chan_groups, capable, pno, blocked))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_build_operation_group() {
        let legit = ChannelGroups {
            band_2ghz: vec![1, 2, 3, 4, 5, 6, 7, 9, 10, 11],
            band_5ghz: vec![
                36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 144, 157,
                161, 165,
            ],
            dfs: vec![52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 144],
            cbw40above: vec![36, 44, 52, 60, 100, 108, 116, 124, 157],
            cbw40below: vec![40, 48, 56, 64, 104, 112, 120, 128, 161],
            cbw80center: vec![42, 58, 106, 122, 139, 155],
            cbw160center: vec![50, 114],
            all: vec![],
        };

        let capable: Vec<u8> = vec![
            1, 3, 4, 5, 6, 7, 8, 9, 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120,
            124, 128, 144, 161,
        ];
        let pno: Vec<u8> = vec![2, 11, 124];
        let blocked: Vec<u8> = vec![144];

        let want = ChannelGroups {
            band_2ghz: vec![1, 3, 4, 5, 6, 7, 9],
            band_5ghz: vec![36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 128, 161],
            dfs: vec![52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 128],
            cbw40above: vec![36, 44, 52, 60, 100, 108, 116],
            cbw40below: vec![40, 48, 56, 64, 104, 112, 120],
            cbw80center: vec![42, 58, 106],
            cbw160center: vec![50],
            all: vec![
                1, 3, 4, 5, 6, 7, 9, 36, 40, 42, 44, 48, 50, 52, 56, 58, 60, 64, 100, 104, 106,
                108, 112, 116, 120, 128, 161,
            ],
        };

        let got = build_operation_group(legit, capable, pno, blocked);
        assert_eq!(want, got);
    }
}
