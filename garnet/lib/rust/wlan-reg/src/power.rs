extern crate toml;

use super::channel;
use super::country;
use super::device_cap;
use super::operclass;
use super::regulation;
use super::regulation::RegulationTable;

use failure::{bail, Error};
use std::collections::HashMap;

#[derive(Debug)]
pub struct PowerBudgetByRange {
    pub chan_idx_beg: u8,
    pub chan_idx_end: u8,
    pub max_conduct_power: i8, // dBm
}

pub fn build_power_budget_by_range(
    table: &RegulationTable,
    role_in_query: &str,
) -> Result<Vec<PowerBudgetByRange>, Error> {
    let mut result: Vec<PowerBudgetByRange> = vec![];

    for s in &table.subband {
        for r in &s.role {
            if r.role != "any" && r.role != role_in_query {
                continue;
            }

            result.push(PowerBudgetByRange {
                chan_idx_beg: s.chan_idx_beg,
                chan_idx_end: s.chan_idx_end,
                max_conduct_power: r.max_conduct_power,
            });
        }
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

pub fn get_power_budget_for_client() -> Result<Vec<(u8, i8)>, Error> {
    let budget = match get_power_budget("client") {
        Err(e) => {
            bail!("failed to get power budget for client: {}", e);
        }
        Ok(b) => b,
    };

    let mut budget_vec: Vec<(u8, i8)> = vec![];
    for (k, v) in budget.iter() {
        budget_vec.push((*k, *v));
    }
    budget_vec.sort();
    Ok(budget_vec)
}

pub fn get_power_budget(role: &str) -> Result<HashMap<u8, i8>, Error> {
    let juris = country::get_active_jurisdiction();

    let operclass_filepath = operclass::get_filepath(&juris);
    let operclass_table = operclass::load_operclasses(&operclass_filepath)?;
    let active_operclasses = device_cap::get_operclasses(juris.as_str())?;
    let chan_groups = channel::build_legitimate_group(&operclass_table, &active_operclasses);

    let reg_filepath = regulation::get_filepath(&juris);
    let reg_table = regulation::load_regulations(&reg_filepath)?;
    let budget_by_range = build_power_budget_by_range(&reg_table, role)?;

    Ok(build_power_budget_by_chan_idx(budget_by_range, chan_groups.all))
}
