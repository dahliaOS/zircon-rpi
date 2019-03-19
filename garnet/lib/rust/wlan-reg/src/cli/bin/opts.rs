//#[macro_use]
extern crate structopt;
use structopt::StructOpt;

#[derive(Debug, StructOpt)]
pub enum Opt {
    #[structopt(name = "show")]
    Show(ShowCommand),
}

#[derive(Debug, StructOpt, Clone)]
pub enum ShowCommand {
    #[structopt(name = "operclass")]
    OperClass {
        #[structopt(raw(required = "true"))]
        jurisdiction: String,
    },

    #[structopt(name = "jurisdictions", help = "show all jurisdictions")]
    Jurisdiction,

    #[structopt(name = "regulation")]
    Regulation {
        #[structopt(raw(required = "true"))]
        jurisdiction: String,
    },
}

//struct Opt {
//    /// Activate debug mode
//    #[structopt(short = "d", long = "debug")]
//    debug: bool,
//    /// Set speed
//    #[structopt(short = "s", long = "speed", default_value = "42")]
//    speed: f64,
//    /// Input file
//    #[structopt(parse(from_os_str))]
//    input: PathBuf,
//    /// Output file, stdout if not present
//    #[structopt(parse(from_os_str))]
//    output: Option<PathBuf>,
//}
