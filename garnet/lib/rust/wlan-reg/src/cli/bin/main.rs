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
        opts::ShowCommand::Jurisdiction => {
            show_jurisdictions();
        }
        opts::ShowCommand::OperClass { jurisdiction } => {
            show_operclass(jurisdiction.as_str());
        }
        opts::ShowCommand::Regulation { jurisdiction } => {
            show_regulation(jurisdiction.as_str());
        }
    };
}

fn get_supported_jurisdictions() -> Result<Vec<String>, Error> {
    let mut result = country::load_supported_jurisdictions()?;
    result.sort();
    Ok(result)
}

fn show_jurisdictions() {
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
