#![allow(unused)]

#[macro_use]
extern crate serde_derive;

extern crate toml;
extern crate wlan_reg;

use failure::Error;
use log::{error, info};
use std::collections::HashMap;
use wlan_reg::*;

fn show_operclass() {
    let juris = country::get_jurisdiction();
    let filepath = operclass::get_filepath(&juris);
    let is_valid = operclass::load_operclasses(filepath.as_str()).is_ok();

    println!(
        "{:20}: {:10} {}",
        "TOML for OperClass",
        if is_valid { "[Valid]" } else { "[Invalid]" },
        filepath
    );
}

fn show_regulation() {
    let juris = country::get_jurisdiction();
    let filepath = regulation::get_filepath(&juris);
    let is_valid = regulation::load_regulations(&filepath.to_string()).is_ok();

    println!(
        "{:20}: {:10} {}",
        "TOML for Regulation",
        if is_valid { "[Valid]" } else { "[Invalid]" },
        filepath
    );
}

fn show_device_cap() {
    let filepath = device_cap::get_filepath();
    let is_valid = device_cap::load_toml(&filepath.to_string()).is_ok();

    let juris = country::get_jurisdiction();
    let active_oper_class_indices = device_cap::get_operating_classes(juris.as_str()).unwrap();

    println!(
        "\n{:20}: {:10} {}",
        "TOML for DeviceCap",
        if is_valid { "[Valid]" } else { "[Invalid]" },
        filepath
    );
    println!("\tDevice OperClasses: {:?}", active_oper_class_indices);
}

fn show_legit_chan_group() {
    let legit_chan_group = channel::get_legitimate_group();
    match legit_chan_group {
        Err(e) => {
            error!("{:?}", e);
            println!("{:?}", e);
        }
        Ok(l) => {
            println!("\n[Channel Groups supported by the device in that Jurisdiction]\n{}", l);
        }
    }
}

fn show_oper_chan_group() {
    let oper_chan_group = channel::get_operation_group();
    match oper_chan_group {
        Err(e) => {
            error!("{:?}", e);
            println!("{:?}", e);
        }
        Ok(l) => {
            println!("\n[Operation Channel Group]\n{}", l);
        }
    }
}

fn show_sample_chan_config() {
    let device_chans: Vec<u8> = match device_cap::get_channels() {
        Err(e) => vec![],
        Ok(v) => v,
    };
    println!("\n[Sample channel configurations]");
    println!("{:20} : {:?}", "Device capabilities", device_chans);
    println!(
        "{:20} : {:?}",
        "Planned Non-operate",
        channel::get_planned_non_operation_chanidx_list()
    );
    println!("{:20} : {:?}", "Blocked", channel::get_blocked_chanidx_list());
}

fn show_power_budget() {
    let budget = match power::get_power_budget_for_client() {
        Err(e) => {
            println!("\n[Power Budget (Chan index: dBm)]\n{}", "[Invalid]");
            return;
        }
        Ok(b) => b,
    };

    let mut budget_vec: Vec<(u8, i8)> = vec![];

    for (k, v) in budget.iter() {
        budget_vec.push((*k, *v));
    }
    budget_vec.sort();

    let mut cnt = 0;
    println!("\n[Power Budget]");
    for (k, v) in budget_vec.iter() {
        print!("Chan {:>4} : {:>3} dBm {:>8}", k, v, "");
        cnt += 1;
        if cnt == 4 {
            println!("");
            cnt = 0;
        }
    }
}

fn show_iso_alpha2() {
    let result = match country::load_iso_alpha2() {
        Err(e) => {
            println!("\n[ISO Alpha2] None known");
            return;
        }
        Ok(a) => a,
    };

    println!("\n[ISO Alpha2]");
    println!("{:?}", result);
}

fn show_sku_table() {
    let sku_name = match sku::read_sku() {
        Ok(s) => s,
        Err(e) => {
            println!("\n[SKU name] got error: {}", e);
            return;
        }
    };
    let sku_info = match sku::get_sku_info(sku_name.clone()) {
        Ok(s) => s,
        Err(e) => {
            println!("\n[SKU Table] got error: {}", e);
            return;
        }
    };

    println!("\n[SKU Table] for {}", sku_name);
    println!("{:#?}", sku_info);
}

fn main() {
    println!("\nFuchsia WLAN Countries and Regulation Test\n");

    let juris = country::get_jurisdiction();
    println!("{:20}: {}", "Jurisdiction", juris);

    show_operclass();
    show_regulation();
    show_device_cap();

    show_legit_chan_group();

    show_sample_chan_config();
    show_oper_chan_group();

    show_power_budget();
    println!("");

    show_iso_alpha2();

    show_sku_table();
}
