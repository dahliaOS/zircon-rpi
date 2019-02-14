extern crate toml;

use super::channel;
use super::country;
use super::device_cap;
use super::operclass;
use super::regulation;

use failure::{bail, Error};
use std::collections::HashMap;
use toml::Value;
use toml::Value::Table;

#[derive(Debug)]
pub struct PowerBudgetByRange {
    pub chan_idx_beg: u8,
    pub chan_idx_end: u8,
    pub max_conduct_power: i8, // dBm
}

pub fn build_power_budget_by_range(
    v: &Value,
    role_in_query: &str,
) -> Result<Vec<PowerBudgetByRange>, Error> {
    let table = match v {
        Table(t) => t,
        _ => {
            bail!("not a table! :{} ", v);
        }
    };

    let mut result: Vec<PowerBudgetByRange> = vec![];
    for (_, elem) in table.iter() {
        if elem.get("do_not_use").is_some() {
            continue;
        }
        let subband_table = match elem {
            Table(s) => s,
            _ => continue,
        };

        let role_table = if subband_table.get(&format!("role-{}", role_in_query)).is_some() {
            &subband_table[&format!("role-{}", role_in_query)]
        } else if subband_table.get("role-any").is_some() {
            &subband_table["role-any"]
        } else {
            continue;
        };

        let chan_idx_beg = subband_table["chan_idx_beg"].as_integer().unwrap() as u8;
        let chan_idx_end = subband_table["chan_idx_end"].as_integer().unwrap() as u8;
        let max_conduct_power = role_table["max_conduct_power"].as_integer().unwrap() as i8;
        result.push(PowerBudgetByRange { chan_idx_beg, chan_idx_end, max_conduct_power });
    }
    Ok(result)
}

pub fn build_power_budget_by_chan_idx(
    budget_by_range: Vec<PowerBudgetByRange>,
    chan_indexes: Vec<u8>,
) -> HashMap<u8, i8> {
    let mut budget_by_chan_idx: HashMap<u8, i8> = HashMap::new();
    for r in budget_by_range.iter() {
        for c in r.chan_idx_beg..=r.chan_idx_end {
            if !chan_indexes.contains(&c) {
                continue;
            }
            budget_by_chan_idx.insert(c, r.max_conduct_power);
        }
    }

    budget_by_chan_idx
}

pub fn get_power_budget_for_client() -> Result<HashMap<u8, i8>, Error> {
    get_power_budget("client")
}

pub fn get_power_budget(role: &str) -> Result<HashMap<u8, i8>, Error> {
    let juris = country::get_jurisdiction();

    let operclass_filepath = operclass::get_filepath(&juris);
    let operclass_toml = operclass::load_toml(&operclass_filepath)?;
    let oper_classes = device_cap::get_operating_classes(juris.as_str())?;
    let chan_groups = channel::build_legitimate_group(&operclass_toml, &oper_classes);

    let reg_filepath = regulation::get_filepath(&juris);
    let reg_toml = regulation::load_toml(&reg_filepath)?;
    let budget_by_range = build_power_budget_by_range(&reg_toml, role)?;

    Ok(build_power_budget_by_chan_idx(budget_by_range, chan_groups.all))
}
