// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![deny(warnings)]

mod file_target;
mod generator;
mod io_packet;
mod issuer;
mod log;
mod operations;
mod sequential_io_generator;
mod verifier;
mod blob_target;

extern crate serde;

use {
    crate::generator::{run_generator, GeneratorArgs},
    crate::log::{log_init, Stats},
    ::log::{debug, log_enabled, Level::Debug},
    failure::Error,
    std::{
        env,
        fs::{metadata, File, OpenOptions},
        io::prelude::*,
        process,
        sync::{Arc, Mutex},
        thread::spawn,
        time::Instant,
    },
    fuchsia_merkle::*,
};

// Magic number that gets written in block header
static MAGIC_NUMBER: u64 = 0x4f6475346573742e;

fn create_blob() {
    let data = vec![0xff; 8192];
    let mut builder = MerkleTreeBuilder::new();
    for _i in 0..8 {
        builder.write(&data[..]);
    }

    let root = builder.finish();
    println!("{}", root.root());

    let blob_name = String::from(format!("/blob/{}", root.root().to_string()));
    println!("{}", blob_name);
    let mut f = File::create(&blob_name).unwrap();
    f.set_len(8192 * 8).unwrap();
    for _i in 0..8 {
        f.write(&data[..]).unwrap();
    }
}

fn create_target(target_name: &String, target_length: u64) {
    let metadata = metadata(&target_name);

    match metadata {
        Ok(stats) => {
            assert!(!stats.permissions().readonly());
            assert!(stats.len() >= target_length);
            return;
        }
        _ => {}
    }
    // TODO(auradkar): File should not be created here. It is generator/target's
    // knowledge/job/responsibility.
    let f = File::create(&target_name).unwrap();
    f.set_len(target_length).unwrap();
}

fn parse_target_name() -> String {
    let args: Vec<String> = env::args().collect();
    args[1].to_string()
}

fn output_config(generator_args_vec: &Vec<GeneratorArgs>, output_config_file: &String) {
    let serialized = serde_json::to_string(&generator_args_vec).unwrap();
    let mut file = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(&output_config_file)
        .unwrap();
    file.write_all(serialized.as_bytes()).unwrap();

    debug!("{}", serialized);
    file.sync_data().unwrap();
}

fn main() -> Result<(), Error> {
    create_blob();
    // These are a bunch of inputs that each generator thread receives. These
    // should be received as input to the app.
    // TODO(auradkar): Implement args parsing and validation logic.
    let issuer_queue_depth: u64 = 40;
    let block_size: u64 = 4096;
    let max_io_size: u64 = 8 * 1024;
    let align: bool = true;
    let max_io_count: u64 = 1000;
    let target_length: u64 = 20 * 1024 * 1024;
    let thread_count: u64 = 3;
    let target_type_file: &str = "target_file";
    let sequential: bool = true;
    let output_config_file: &str = "/tmp/output.config";

    let start_instant: Instant = Instant::now();
    log_init().unwrap();

    let mut thread_handles = vec![];
    let mut generator_args_vec = vec![];

    let file_name = parse_target_name();
    create_target(&file_name, target_length);

    let metadata = metadata(&file_name)?;
    // lazy_static::initialize(&start_instant);

    let mut offset_start = 0 as u64;
    let range_size = (metadata.len() / thread_count as u64) as u64;

    // To keep contention among threads low, each generator owns/updates their
    // stats. The "main" thread holds a
    // reference to these stats so that, when ready, it can print the
    // progress/stats from time to time.
    let mut stats_array: Vec<Arc<Mutex<Stats>>> = Vec::with_capacity(thread_count as usize);

    for i in 0..thread_count {
        let args = GeneratorArgs::new(
            MAGIC_NUMBER,
            process::id() as u64,
            i, // generator id
            block_size,
            max_io_size,
            align,
            i, // seed
            file_name.to_string(),
            offset_start..(offset_start + range_size),
            target_type_file.to_string(),
            issuer_queue_depth,
            max_io_count,
            sequential,
        );
        generator_args_vec.push(args.clone());

        let stats = {
            let mut stats = Stats::new();
            stats.start_clock();
            Arc::new(Mutex::new(stats))
        };
        stats_array.push(stats.clone());
        thread_handles.push(spawn(move || run_generator(args, start_instant.clone(), stats)));
        offset_start += range_size;
    }

    output_config(&generator_args_vec, &output_config_file.to_string());
    for handle in thread_handles {
        handle.join().unwrap()?;
    }

    let mut aggregate_stats = Stats::new();
    let mut i = 0;

    // How the summary stats. For long running IO load we should print the stats
    // from time to time to show how IO are going. A TODO(auradkar).
    for stat in stats_array {
        let stat = stat.lock().unwrap();
        aggregate_stats.aggregate_summary(&stat);
        if log_enabled!(Debug) {
            debug!("===== For generator-{} =====", i);
            stat.display_summary();
            i += 1;
        }
    }
    println!("===== Aggregate Stats =====");
    aggregate_stats.display_summary();

    Ok(())
}
