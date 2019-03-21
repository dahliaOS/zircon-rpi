// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
        name = "channel-groups",
        about = "Shows channel groups for the underlying device in the active jurisdiction"
    )]
    ChannelGroups,

    #[structopt(name = "device-meta", about = "Shows device meta capabilities")]
    DeviceMeta,

    #[structopt(name = "jurisdictions-all", about = "Shows all jurisdictions supported")]
    JurisdictionsAll,

    #[structopt(name = "jurisdiction-active", about = "Shows the active jurisdiction")]
    JurisdictionsActive,

    #[structopt(
        name = "operclass",
        about = "Shows operating classes defined in the specified jurisdiction"
    )]
    OperClass {
        #[structopt(raw(required = "true"))]
        jurisdiction: String,
    },

    #[structopt(name = "power-budget", about = "Shows power budget in the active jurisdiction")]
    PowerBudget,

    #[structopt(
        name = "regulation",
        about = "Shows regulations defined in the specified jurisdiction"
    )]
    Regulation {
        #[structopt(raw(required = "true"))]
        jurisdiction: String,
    },
}
