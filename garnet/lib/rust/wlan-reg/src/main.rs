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
    //    println!("  Parse result:\n{:?}\n", toml);
    //    println!("   From file contents:");
    //    utils::dump_file(&filepath);

    let channel_groups =
        channel::build_legit_channel_groups(&toml, &country::get_active_operating_classes());

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

    // println!("{:#?}", toml);
    let budget_by_range = power::build_power_budget_by_range(&toml, "client");
    match budget_by_range {
        Err(e) => error!("{:?}", e),
        Ok(r) => {
            for e in r.iter() {
                println!("{:?}", e)
            }
        }
    }
}

fn main() {
    let legit_chan_group = channel::get_legit_chan_groups();
    match legit_chan_group {
        Err(e) => {
            error!("{:?}", e);
            println!("{:?}", e);
        }
        Ok(l) => {
            println!("\nLegit Channel Group\n{}", l);
        }
    }

    println!("Device capable channels   {:?}", channel::get_device_capable_chanidx_list());
    println!("Planned non-oper channels {:?}", channel::get_planned_non_operation_chanidx_list());
    println!("Blocked channels          {:?}", channel::get_blocked_chanidx_list());

    let oper_chan_group = channel::get_oper_chan_groups();
    match oper_chan_group {
        Err(e) => {
            error!("{:?}", e);
            println!("{:?}", e);
        }
        Ok(l) => {
            println!("\nOperation Channel Group\n{}", l);
        }
    }

    let budget = power::get_power_budget_for_client();
    match budget {
        Err(e) => {
            error!("{:?}", e);
            println!("{:?}", e)
        }
        Ok(b) => {
            println!("Power Budget By Channel Index\n{:?}", b);
        }
    }
}
