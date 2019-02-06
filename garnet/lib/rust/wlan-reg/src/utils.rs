extern crate log;
extern crate serde_derive;
extern crate toml;

use failure::{bail, Error};
use log::{error, info};
use std::{fs, fs::File, io::prelude::*, path::PathBuf};
use toml::Value;
use toml::Value::Table;

pub fn dump_file(filepath: &String) -> Result<(), Error> {
    let mut file = File::open(&filepath)?;
    let mut contents = String::new();
    file.read_to_string(&mut contents)?;

    println!("\n[file] {}\n", filepath);
    println!("{}", contents);
    Ok(())
}
