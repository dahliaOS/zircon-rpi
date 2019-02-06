#![allow(unused)]
use log::{error, info};

use failure::Error;

#[macro_use]

mod loader;
mod country;
mod utils;

fn main() {
    let jurisdiction = country::get_jurisdiction();
    let filepath = loader::get_operating_class_filename(&jurisdiction);

    let toml = match loader::load_operating_class_toml(&filepath.to_string()) {
        Err(e) => {
            error!("{}", e);
            return;
        }
        Ok(t) => t,
    };

    println!("\nFor jurisdiction: {}", jurisdiction);
    println!("  File: {}", filepath);
    println!("  Parse result:\n{:?}\n", toml);
    println!("   From file contents:");
    utils::dump_file(&filepath);
}
