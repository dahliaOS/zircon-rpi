//#[macro_use]
extern crate structopt;
use structopt::StructOpt;
mod opts;

extern crate wlan_reg;
use failure::Error;
use wlan_reg::*;
//use log::{error, info};

fn main() {
    let opt = opts::Opt::from_args();

    println!("{:?}", opt);

    match opt {
        opts::Opt::Show(cmd) => show(cmd),
    }
}

fn show(cmd: opts::ShowCommand) {
    match cmd {
        opts::ShowCommand::AllJurisdictions => {
            show_all_jurisdictions();
        }
        opts::ShowCommand::ActiveJurisdiction => {
            show_active_jurisdiction();
        }
        opts::ShowCommand::OperClass { jurisdiction } => {
            show_operclass(jurisdiction.as_str());
        }
        opts::ShowCommand::Regulation { jurisdiction } => {
            show_regulation(jurisdiction.as_str());
        }
        opts::ShowCommand::DeviceMeta => {
            show_device_meta();
        }
        opts::ShowCommand::PowerBudget => {
            show_power_budget();
        }
        opts::ShowCommand::ChannelGroups => {
            show_channel_groups();
        }
    };
}

fn get_supported_jurisdictions() -> Result<Vec<String>, Error> {
    let mut result = country::load_supported_jurisdictions()?;
    result.sort();
    Ok(result)
}

fn show_all_jurisdictions() {
    let jurisdictions = match get_supported_jurisdictions() {
        Err(e) => {
            println!("\nError: Failed to load jurisdiction file: {}", e);
            return;
        }
        Ok(j) => j,
    };

    println!("\nAll jurisdictions");

    let mut items_per_line = 0;
    for j in jurisdictions {
        print!("{:>8}", j);
        items_per_line += 1;
        if items_per_line == 8 {
            items_per_line = 0;
            println!("");
        }
    }
}

fn show_active_jurisdiction() {
    println!(
        "\nActive jurisdiction: {} (faked for development)\n",
        country::get_active_jurisdiction()
    );
}

fn show_operclass(jurisdiction: &str) {
    // TODO(porce): Change this to Jurisdiction from ISO alpha2
    let result = match get_supported_jurisdictions() {
        Err(e) => {
            println!("\nError: Failed to load jurisdiction file: {}", e);
            return;
        }
        Ok(a) => a,
    };

    if !result.contains(&jurisdiction.to_string()) {
        println!("\nError: jurididction {} unknown", jurisdiction);
        return;
    }

    let filepath_juris = operclass::get_filepath(jurisdiction);
    let operclasses = match operclass::load_operclasses(filepath_juris.as_str()) {
        Err(e) => {
            // Unexpected situation, since get_supported_jurisdictions()
            // already passed the validation.
            println!(
                "jurisdiction {} does not have a corresponding and valid OperClass file: {}",
                jurisdiction, e
            );
            return;
        }
        Ok(o) => o,
    };

    println!("\nFor juridiction {}\n", jurisdiction);
    println!("{:#?}", operclasses);
}

fn show_regulation(jurisdiction: &str) {
    // TODO(porce): Change this to Jurisdiction from ISO alpha2
    let result = match get_supported_jurisdictions() {
        Err(e) => {
            println!("\nError: Failed to load jurisdiction file: {}", e);
            return;
        }
        Ok(a) => a,
    };

    if !result.contains(&jurisdiction.to_string()) {
        println!("\nError: jurididction {} unknown", jurisdiction);
        return;
    }

    let filepath_reg = regulation::get_filepath(jurisdiction);
    let reg = match regulation::load_regulations(&filepath_reg) {
        Err(e) => {
            // Unexpected situation, since get_supported_jurisdictions()
            // already passed the validation.
            println!(
                "jurisdiction {} does not have a corresponding and valid Regulation file: {}",
                jurisdiction, e
            );
            return;
        }
        Ok(r) => r,
    };
    println!("\nFor juridiction {}\n", jurisdiction);
    println!("{:#?}", reg);
}

fn show_device_meta() {
    let filepath = device_cap::get_filepath();
    let device_meta = match device_cap::load_device_caps(&filepath.to_string()) {
        Err(e) => {
            println!("cannot find device meta capabilities for underlying device: {}", e);
            return;
        }
        Ok(d) => d,
    };
    println!("\nFor device meta capability\n");
    println!("{:#?}", device_meta);
}

fn show_power_budget() {
    let budget_vec = match power::get_power_budget_for_client() {
        Err(e) => {
            println!("\n[Power Budget] for Client role");
            println!("{}", e);
            return;
        }
        Ok(b) => b,
    };

    let mut cnt = 0;
    println!("\n[Power Budget] for Client role");
    for (k, v) in budget_vec.iter() {
        print!("Chan {:>3}: {:>2} dBm {:>6}", k, v, "");
        cnt += 1;
        if cnt == 6 {
            println!("");
            cnt = 0;
        }
    }
}

fn show_channel_groups() {
    println!("\nActive jurisdiction: {}", country::get_active_jurisdiction());
    println!("\n[Channel Groups (Legitimate)]");
    match channel::get_legitimate_group() {
        Err(e) => {
            println!("{}", e);
        }
        Ok(l) => {
            println!("{}", l);
        }
    }

    println!("\n[Channel Groups (Config, DeviceCap)] (faked for development)");
    match device_cap::get_channels() {
        Err(e) => {
            println!("{:20} : {}", "Device capabilities", e);
        }
        Ok(v) => {
            println!("{:20} : {:?}", "Device capabilities", v);
        }
    };
    println!(
        "{:20} : {:?}",
        "Planned Non-operate",
        channel::get_planned_non_operation_chanidx_list()
    );
    println!("{:20} : {:?}", "Blocked", channel::get_blocked_chanidx_list());

    println!("\n[Channel Groups (Active)]");
    let oper_chan_group = channel::get_operation_group();
    match oper_chan_group {
        Err(e) => {
            println!("{}", e);
        }
        Ok(l) => {
            println!("{}", l);
        }
    }
}
