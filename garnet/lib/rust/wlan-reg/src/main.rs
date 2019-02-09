#![allow(unused)]

extern crate wlan_reg;

use failure::Error;
use log::{error, info};
use wlan_reg::*;

fn play_operating_class() {
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

    let channel_groups =
        channel::build_channel_groups(&toml, &country::get_active_operating_classes());

    println!("{}", channel_groups);
}

fn play_regulation() {
    let jurisdiction = country::get_jurisdiction();
    let filepath = loader::get_regulation_filename(&jurisdiction);

    let toml = match loader::load_regulation_toml(&filepath.to_string()) {
        Err(e) => {
            error!("{}", e);
            println!("{}", e);
            return;
        }
        Ok(t) => t,
    };

    println!("{:#?}", toml);
}

fn main() {
    play_regulation();
}
