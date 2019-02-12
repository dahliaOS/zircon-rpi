extern crate toml;

use std::fmt;
use toml::Value;

pub struct ChannelGroups {
    pub band_2ghz: Vec<u8>,
    pub band_5ghz: Vec<u8>,
    pub dfs: Vec<u8>,
    pub cbw40above: Vec<u8>,
    pub cbw40below: Vec<u8>,
    pub cbw80center: Vec<u8>,
    pub cbw160center: Vec<u8>,
    pub all: Vec<u8>,
}

impl fmt::Display for ChannelGroups {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "Channel Groups\n{:14}: {:?}\n{:14}: {:?}\n{:14}: {:?}\n{:14}: {:?}\n{:14}: {:?}\n{:14}: {:?}\n{:14}: {:?}\n{:14}: {:?}\n",
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

fn is_set(v: &Value, key1: &String, key2: &str) -> bool {
    if v.get(key1).is_none() || v[key1].get(key2).is_none() {
        return false;
    }
    v[key1][key2].as_bool().unwrap()
}

/// Converts TOML integer array into Vec<u8>
/// TODO(porce): Many assumptions are made on the input parameter.
fn get_chanlist(v: &Value) -> Vec<u8> {
    let mut result: Vec<u8> = vec![];

    for e in v.as_array().unwrap() {
        result.push(e.as_integer().unwrap() as u8);
    }
    result
}

// Takes already validated TOMM  value, returns ChannelGroups
pub fn build_channel_groups(v: &Value, active_operclasses: &Vec<u8>) -> ChannelGroups {
    let mut dfs = vec![];
    let mut band_2ghz = vec![];
    let mut band_5ghz = vec![];
    let mut cbw40above = vec![];
    let mut cbw40below = vec![];
    let mut cbw80center = vec![];
    let mut cbw160center = vec![];

    // TODO(porce): Improve the walk by walking on the BTreeMap.
    let mut operclass_cnt = 0;
    const OPERCLASS_IDX_MIN: u8 = 1;
    const OPERCLASS_IDX_MAX: u8 = 255;
    for idx in OPERCLASS_IDX_MIN..=OPERCLASS_IDX_MAX {
        if !active_operclasses.contains(&idx) {
            continue;
        }
        let mut key = format!("{}-{}", v["jurisdiction"], idx);
        key = str::replace(key.as_str(), "\"", "");
        if v.get(&key).is_none() {
            continue;
        }

        operclass_cnt += 1;

        let channel_set = get_chanlist(&v[&key]["set"]);
        let center_channel_set = get_chanlist(&v[&key]["center_freq_idx"]);

        let start_freq = v[&key]["start_freq"].as_float().unwrap() as f64;
        let spacing = v[&key]["spacing"].as_integer().unwrap() as u8;

        if start_freq == 5.000 {
            band_5ghz.extend(&channel_set);
        }

        if start_freq == 2.407 {
            band_2ghz.extend(&channel_set);
        }
        if is_set(v, &key, "dfs_50_100") {
            dfs.extend(&channel_set);
        }

        if is_set(v, &key, "primary_chan_lower") {
            cbw40above.extend(&channel_set);
        }
        if is_set(v, &key, "primary_chan_upper") {
            cbw40below.extend(&channel_set);
        }
        if spacing == 80 {
            cbw80center.extend(&center_channel_set);
        }
        if spacing == 160 {
            cbw160center.extend(&center_channel_set);
        }
    }

    if operclass_cnt == 0 {
        // error!("The input Value carries no operating class. Are you sure?");
        println!("The input Value carries no operating class. Are you sure?");
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
