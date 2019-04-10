// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(warnings)]

use {
    crate::generator::ActiveCommands,
    crate::io_packet::{InternalCommand, IoPacketType},
    crate::operations::PipelineStages,
    failure::Error,
    log::{debug, error, warn},
    std::{
        process,
        sync::mpsc::{Receiver, Sender},
    },
};

pub struct IssuerArgs {
    /// Human friendly name for this thread.
    name: String,

    /// Unique identifier for each generator.
    issuer_unique_id: u64,

    // Issuer's stage in lifetime of an IO.
    stage: PipelineStages,

    // Channel used to receive commands from generator
    from_generator: Receiver<IoPacketType>,

    // Channel used to send commands to verifier
    to_verifier: Sender<IoPacketType>,

    // Channel used to receive commands from verifier
    from_verifier: Receiver<IoPacketType>,

    // Way for generator and verifier to notify the issuer that there are one or
    // more commands in the queue.
    active_commands: ActiveCommands,
}

impl IssuerArgs {
    pub fn new(
        base_name: String,
        issuer_unique_id: u64,
        from_generator: Receiver<IoPacketType>,
        to_verifier: Sender<IoPacketType>,
        from_verifier: Receiver<IoPacketType>,
        active_commands: ActiveCommands,
    ) -> IssuerArgs {
        IssuerArgs {
            name: String::from(format!("{}-{}", base_name, issuer_unique_id)),
            issuer_unique_id: issuer_unique_id,
            from_generator: from_generator,
            to_verifier: to_verifier,
            from_verifier: from_verifier,
            stage: PipelineStages::Issue,
            active_commands: active_commands,
        }
    }
}

pub fn run_issuer(mut args: IssuerArgs) -> Result<(), Error> {
    // Even on happy path, either generator or verifier can be the first to
    // close the channel. These two variables keep track of whether the channel
    // was closed or not.
    let mut scan_generator = true;
    let mut scan_verifier = true;

    // True if the current command is from verifier.
    let mut verifying_cmd:bool;

    let mut cmd_or_err;
    let mut cmd;

    // This thread/loop is not done till we hear explicitly from generator and
    // from verifier that they both are done. We keep track of who is done.
    while scan_generator || scan_verifier {
        verifying_cmd = false;

        // May block
        args.active_commands.decrement();

        // There is at least one command in the queues. We don't know in which
        // yet.
        // Lets give priority to io packets from verifier.
        cmd_or_err = args.from_verifier.try_recv();
        if !cmd_or_err.is_err() {
            verifying_cmd = true;
        }

        // if we found a command on verifier channel, don't look for command on
        // generator channel
        if !verifying_cmd {
            cmd_or_err = args.from_generator.try_recv();
        }

        cmd = cmd_or_err.unwrap();

        debug!(
            "from issuer: {} id: {} io_seq: {} op: {:?} verifying_cmd: {}",
            args.name,
            args.issuer_unique_id,
            cmd.sequence_number(),
            cmd.operation_type(),
            verifying_cmd,
        );

        cmd.timestamp_stage_start(&args.stage);
        cmd.do_io();
        if !cmd.is_complete() {
            error!("Asynchronous commands not implemented yet.");
            process::abort();
        }

        // Mark done timestamps.
        cmd.timestamp_stage_end(&args.stage);

        // Cloning the command
        let internal_command = cmd.abort_or_exit();

        // Check if this was an internal command and if so take appropriate
        // action.
        match internal_command {
            InternalCommand::Exit => {
                if verifying_cmd {
                    scan_verifier = false;
                    // if this internal command is coming from verifier,
                    // skip sending it to verifier.
                    continue;
                } else {
                    scan_generator = false;
                }
                debug!("{} - clean exit", args.name);
            }
            InternalCommand::Abort => {
                warn!("{} - aborted", args.name);
                break;
            }
            InternalCommand::None => {}
        }

        if args.to_verifier.send(cmd).is_err() {
            error!("error sending command from issuer");
            process::abort();
        }
    }

    Ok(())
}
