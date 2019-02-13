use failure::{bail, Error};
use std::{fs, fs::File, io::prelude::*, path::PathBuf};
use toml::Value;
use toml::Value::Table;

pub fn load_toml(filepath: &str) -> Result<Value, Error> {
    let rel_path = PathBuf::from(filepath);
    let _abs_path = fs::canonicalize(&rel_path)?;
    let mut file = File::open(filepath)?;
    let mut contents = String::new();
    file.read_to_string(&mut contents)?;
    let value = contents.parse::<Value>()?;

    match value {
        Table(_) => Ok(value),
        _ => {
            bail!("Parsed TOML file is not a Table: {}", filepath);
        }
    }
}

pub fn dump_file(filepath: &String) -> Result<(), Error> {
    let mut file = File::open(&filepath)?;
    let mut contents = String::new();
    file.read_to_string(&mut contents)?;

    println!("\n[file] {}\n", filepath);
    println!("{}", contents);
    Ok(())
}

pub fn is_set(v: &Value, key1: &String, key2: &str) -> bool {
    if v.get(key1).is_none() || v[key1].get(key2).is_none() {
        return false;
    }
    v[key1][key2].as_bool().unwrap()
}

/// Converts TOML integer array into Vec<u8>
/// TODO(porce): Many assumptions are made on the input parameter.
pub fn get_chanlist(v: &Value) -> Vec<u8> {
    let mut result: Vec<u8> = vec![];

    for e in v.as_array().unwrap() {
        result.push(e.as_integer().unwrap() as u8);
    }
    result
}
