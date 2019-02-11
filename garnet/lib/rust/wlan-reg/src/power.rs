extern crate toml;

use toml::Value;
use toml::Value::Table;

use failure::{bail, Error};

#[derive(Debug)]
pub struct PowerBudgetByRange {
    pub chan_idx_beg: u8,
    pub chan_idx_end: u8,
    pub max_conduct_power: i8, // dBm
}

pub fn build_power_budget(
    v: &Value,
    role_in_query: String,
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
