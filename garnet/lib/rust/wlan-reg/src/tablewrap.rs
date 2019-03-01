extern crate toml;

//use failure::{bail, Error};
use super::utils;
use toml::value::Table;
use toml::Value;

#[derive(Debug)]
pub struct TableWrap {
    pub table: Table,
    pub is_table_valid: bool,
}

impl TableWrap {
    pub fn new(filepath: &str) -> Self {
        let result = match utils::load_toml(filepath) {
            Err(_) => TableWrap { table: Table::new(), is_table_valid: false },
            Ok(t) => TableWrap { table: t, is_table_valid: true },
        };
        result
    }

    pub fn get_bool(&self, key: &str) -> bool {
        // false if the key is absent
        self.table.get(key).is_some() && self.table[key].as_bool().unwrap()
    }

    pub fn get_vec_u8(&self, key: &str) -> Vec<u8> {
        let mut result: Vec<u8> = vec![];
        if self.table.get(key).is_none() {
            return result;
        }

        for elem in self.table[key].as_array().unwrap() {
            result.push(elem.as_integer().unwrap() as u8);
        }
        result
    }

    pub fn get_vec_str(&self, key: &str) -> Vec<String> {
        let mut result: Vec<String> = vec![];
        if self.table.get(key).is_none() {
            return result;
        }

        for elem in self.table[key].as_array().unwrap() {
            result.push(elem.as_str().unwrap().to_string());
        }

        result
    }

    pub fn get_nested(&self, key: &str) -> TableWrap {
        if self.table.get(key).is_none() {
            return TableWrap { table: Table::new(), is_table_valid: false };
        }

        match &self.table[key] {
            Value::Table(t) => TableWrap { table: t.clone(), is_table_valid: true },
            _ => TableWrap { table: Table::new(), is_table_valid: false },
        }
    }
}
