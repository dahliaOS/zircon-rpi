use failure::Error;
use std::{fs::File, io::prelude::*};

pub fn dump_file(filepath: &String) -> Result<(), Error> {
    let mut file = File::open(&filepath)?;
    let mut contents = String::new();
    file.read_to_string(&mut contents)?;

    println!("\n[file] {}\n", filepath);
    println!("{}", contents);
    Ok(())
}
