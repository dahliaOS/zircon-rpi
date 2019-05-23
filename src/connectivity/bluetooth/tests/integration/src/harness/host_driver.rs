// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{err_msg, Error, ResultExt},
    fidl_fuchsia_bluetooth_control::{AdapterInfo, AdapterState, RemoteDevice},
    fidl_fuchsia_bluetooth_host::{HostEvent, HostProxy},
    fuchsia_async::{self as fasync, TimeoutExt},
    fuchsia_bluetooth::{
        error::Error as BtError,
        expectation::{
            asynchronous::{ExpectableState, ExpectableStateExt, ExpectationHarness},
            Predicate,
        },
        fake_hci::FakeHciDevice,
        hci, host,
        util::{clone_host_info, clone_host_state, clone_remote_device},
    },
    fuchsia_vfs_watcher::{WatchEvent as VfsWatchEvent, Watcher as VfsWatcher},
    fuchsia_zircon::{Duration, DurationNum},
    futures::{Future, TryFutureExt, TryStreamExt},
    std::{borrow::Borrow, collections::HashMap, fs::File, path::PathBuf},
};

use crate::harness::TestHarness;

const BT_HOST_DIR: &str = "/dev/class/bt-host";
const TIMEOUT_SECONDS: i64 = 10; // in seconds

pub fn timeout_duration() -> Duration {
    TIMEOUT_SECONDS.seconds()
}

// Applies `delta` to `base`.
fn apply_delta(base: AdapterState, delta: AdapterState) -> AdapterState {
    AdapterState {
        local_name: delta.local_name.or(base.local_name),
        discoverable: delta.discoverable.or(base.discoverable),
        discovering: delta.discovering.or(base.discovering),
        local_service_uuids: delta.local_service_uuids.or(base.local_service_uuids),
    }
}

// Returns a Future that resolves when a bt-host device gets added under the given topological
// path.
async fn watch_for_new_host_helper(
    mut watcher: VfsWatcher,
    parent_topo_path: String,
) -> Result<(File, PathBuf), Error> {
    while let Some(msg) = await!(watcher.try_next())? {
        match msg.event {
            VfsWatchEvent::EXISTING | VfsWatchEvent::ADD_FILE => {
                let path =
                    PathBuf::from(format!("{}/{}", BT_HOST_DIR, msg.filename.to_string_lossy()));
                let host_fd = hci::open_rdwr(&path)?;
                let host_topo_path = fdio::device_get_topo_path(&host_fd)?;
                if host_topo_path.starts_with(parent_topo_path.as_str()) {
                    return Ok((host_fd, path.clone()));
                }
            }
            _ => (),
        }
    }
    unreachable!();
}

// Returns a Future that resolves when the bt-host device with the given path gets removed.
async fn wait_for_host_removal_helper(mut watcher: VfsWatcher, path: String) -> Result<(), Error> {
    while let Some(msg) = await!(watcher.try_next())? {
        match msg.event {
            VfsWatchEvent::REMOVE_FILE => {
                let actual_path = format!("{}/{}", BT_HOST_DIR, msg.filename.to_string_lossy());
                if path == actual_path {
                    return Ok(());
                }
            }
            _ => (),
        }
    }
    unreachable!();
}

async fn watch_for_host(watcher: VfsWatcher, hci_path: String) -> Result<(File, PathBuf), Error> {
    await!(watch_for_new_host_helper(watcher, hci_path)
        .on_timeout(timeout_duration().after_now(), move || Err(err_msg(
            "timed out waiting for bt-host"
        ))))
}

async fn wait_for_host_removal(watcher: VfsWatcher, path: String) -> Result<(), Error> {
    await!(wait_for_host_removal_helper(watcher, path)
        .on_timeout(timeout_duration().after_now(), move || Err(err_msg(
            "timed out waiting for bt-host removal"
        ))))
}

pub fn expect_eq<T>(expected: &T, actual: &T) -> Result<(), Error>
where
    T: std::fmt::Debug + std::cmp::PartialEq,
{
    if *expected == *actual {
        Ok(())
    } else {
        Err(BtError::new(&format!("failed - expected '{:#?}', found: '{:#?}'", expected, actual))
            .into())
    }
}

macro_rules! expect_eq {
    ($expected:expr, $actual:expr) => {
        expect_eq(&$expected, &$actual)
    };
}

macro_rules! expect_true {
    ($condition:expr) => {
        if $condition{
            Ok(())
        } else {
            Err(fuchsia_bluetooth::error::Error::new(&format!(
                "condition is not true: {}",
                stringify!($condition)
            )).into())
        } as Result<(), Error>
    }
}

pub fn expect_remote_device(
    test_state: &HostDriverHarness,
    address: &str,
    expected: &Predicate<RemoteDevice>,
) -> Result<(), Error> {
    let state = test_state.read();
    let peer = state
        .peers
        .values()
        .find(|dev| dev.address == address)
        .ok_or(BtError::new(&format!("Peer with address '{}' not found", address)))?;
    expect_true!(expected.satisfied(peer))
}

pub type HostDriverHarness = ExpectationHarness<HostState, (HostProxy, Option<FakeHciDevice>)>;

pub struct HostState {
    // Access to the bt-host device under test.
    host_path: String,

    // Current bt-host driver state.
    host_info: AdapterInfo,

    // All known remote devices, indexed by their identifiers.
    peers: HashMap<String, RemoteDevice>,
}

impl Clone for HostState {
    fn clone(&self) -> HostState {
        HostState {
            host_path: self.host_path.clone(),
            host_info: clone_host_info(&self.host_info),
            peers: self.peers.iter().map(|(k, v)| (k.clone(), clone_remote_device(v))).collect(),
        }
    }
}

// Creates a fake bt-hci device and returns the corresponding bt-host device once it gets created.
async fn new_host_harness() -> Result<HostDriverHarness, Error> {
    let fake_hci_dev = FakeHciDevice::new("bt-hci-integration-test-0")?;
    let fake_hci_topo_path = fdio::device_get_topo_path(fake_hci_dev.file())?;

    let dir = File::open(&BT_HOST_DIR)?;
    let watcher = VfsWatcher::new(&dir)?;
    let (host_dev_fd, path) = await!(watch_for_host(watcher, fake_hci_topo_path))?;

    // Open a Host FIDL interface channel to the bt-host device.
    let fidl_handle = host::open_host_channel(&host_dev_fd)?;
    let host_proxy = HostProxy::new(fasync::Channel::from_channel(fidl_handle.into())?);

    let host_info = await!(host_proxy.get_info())?;
    let host_path = path.to_string_lossy().to_string();
    let peers = HashMap::new();

    Ok(ExpectationHarness::init(
        (host_proxy, Some(fake_hci_dev)),
        HostState { host_path, host_info, peers },
    ))
}

// Returns a Future that resolves when the state of any RemoteDevice matches `target`.
pub async fn expect_host_peer(
    host: &HostDriverHarness,
    target: Predicate<RemoteDevice>,
) -> Result<HostState, Error> {
    await!(host.when_satisfied(
        Predicate::<HostState>::new(
            move |host| { host.peers.iter().any(|(_, p)| target.satisfied(p)) },
            None
        ),
        timeout_duration()
    ))
}

pub async fn expect_no_host_peer(
    host: &HostDriverHarness,
    target: Predicate<RemoteDevice>,
) -> Result<HostState, Error> {
    await!(host.when_satisfied(
            Predicate::<HostState>::new(
                move |host| { host.peers.iter().all(|(_, p)| !target.satisfied(p)) },
                None
            ),
            timeout_duration()
    ))
}

pub async fn expect_adapter_state(
    host: &HostDriverHarness,
    target: Predicate<AdapterState>,
) -> Result<HostState, Error> {
    await!(host.when_satisfied(
        Predicate::<HostState>::new(
            move |host| {
                match &host.host_info.state {
                    Some(state) => target.satisfied(state),
                    None => false,
                }
            },
            None,
        ),
        timeout_duration()
    ))
}

impl TestHarness for HostDriverHarness {
    fn run_with_harness<F, Fut>(test_func: F) -> Result<(), Error>
    where
        F: FnOnce(Self) -> Fut,
        Fut: Future<Output = Result<(), Error>>,
    {
        let mut executor = fasync::Executor::new().context("error creating event loop")?;
        executor.run_singlethreaded(run_host_harness_test_async(test_func))
    }
}

// Returns a Future that handles Host interface events.
async fn handle_host_events(harness: HostDriverHarness) -> Result<(), Error> {
    let mut events = harness.aux().0.take_event_stream();

    while let Some(e) = await!(events.try_next())? {
        match e {
            HostEvent::OnAdapterStateChanged { state } => {
                let host_info = &mut harness.write_state().host_info;
                let base = match host_info.state {
                    Some(ref state) => clone_host_state(state.borrow()),
                    None => AdapterState {
                        local_name: None,
                        discoverable: None,
                        discovering: None,
                        local_service_uuids: None,
                    },
                };
                let new_state = apply_delta(base, state);
                host_info.state = Some(Box::new(new_state));
            }
            HostEvent::OnDeviceUpdated { device } => {
                harness.write_state().peers.insert(device.identifier.clone(), device);
            }
            HostEvent::OnDeviceRemoved { identifier } => {
                harness.write_state().peers.remove(&identifier);
            }
            // TODO(armansito): handle other events
            e => {
                eprintln!("Unhandled event: {:?}", e);
            }
        }
        harness.notify_state_changed();
    }

    Ok(())
}

async fn run_host_harness_test_async<F, Fut>(test_func: F) -> Result<(), Error>
where
    F: FnOnce(HostDriverHarness) -> Fut,
    Fut: Future<Output = Result<(), Error>>,
{
    let host_test = await!(new_host_harness())?;

    // Start processing events in a background task.
    fasync::spawn(
        handle_host_events(host_test.clone())
            .unwrap_or_else(|e| eprintln!("Error handling host events: {:?}", e)),
    );

    // Run the test and obtain the test result.
    let result = await!(test_func(host_test.clone()));

    // Shut down the fake bt-hci device and make sure the bt-host device gets removed.
    let dir = File::open(&BT_HOST_DIR)?;
    let watcher = VfsWatcher::new(&dir)?;
    host_test.aux().1 = None;
    await!(wait_for_host_removal(watcher, host_test.read().host_path.clone()))?;
    result
}
