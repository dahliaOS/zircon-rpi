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
    #[structopt(
        name = "operclass",
        about = "Shows operating classes defined in the specified jurisdiction."
    )]
    OperClass {
        #[structopt(raw(required = "true"))]
        jurisdiction: String,
    },

    #[structopt(name = "jurisdictions-all", about = "Shows all jurisdictions supported")]
    AllJurisdictions,

    #[structopt(
        name = "regulation",
        about = "Shows regulations defined in the specified jurisdiction."
    )]
    Regulation {
        #[structopt(raw(required = "true"))]
        jurisdiction: String,
    },

    #[structopt(name = "device-meta", about = "Shows device meta capabilities")]
    DeviceMeta,

    #[structopt(name = "jurisdiction-active", about = "Shows the active jurisdiction")]
    ActiveJurisdiction,

    #[structopt(name = "power-budget", about = "Shows power budget in the active jurisdiction")]
    PowerBudget,

    #[structopt(
        name = "channel-groups",
        about = "Shows channel groups for the underlying device in the active jurisdiction"
    )]
    ChannelGroups,
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
