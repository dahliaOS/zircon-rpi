// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::{Error, err_msg};
use fuchsia_async as fasync;
use fuchsia_bluetooth::host::open_host_channel;
use fuchsia_bluetooth::util::clone_host_info;
use fidl_fuchsia_bluetooth_control::AdapterInfo;
use fidl_fuchsia_bluetooth_host::HostProxy;
//use fidl_fuchsia_bluetooth_control::PairingDelegateMarker;
//use fidl_fuchsia_bluetooth_control::{InputCapabilityType, OutputCapabilityType};
//use fuchsia_syslog::{fx_log, fx_log_err, fx_log_info};
use std::path::{Path, PathBuf};

//use fidl_fuchsia_bluetooth_host::HostEvent::*;
//use fidl_fuchsia_bluetooth_host::HostEvent;

use futures::future::FutureExt;
use futures::channel::oneshot;
use futures::channel::oneshot::Receiver;
use std::fs::File;

//use crate::host_dispatcher::*;
use crate::types::bt;
use crate::types::*;

/// A type that supports controlling discovery
pub trait Discovery {
    fn start_discovery(&mut self) -> Receiver<bt::Result<()>>;
    fn stop_discovery(&self) -> Receiver<bt::Result<()>>;
}

/// A type that represents the BT Host subsystem
pub trait Host : Discovery + Send + Sync + 'static {
    fn path(&self) -> &Path;
}

/// Type representing a BT Adapter with a loaded Host Device Driver
pub struct FidlHost {
    device_path: PathBuf,
    proxy: HostProxy,
    info: AdapterInfo
}

impl FidlHost {
    pub fn get_info(&self) -> AdapterInfo { clone_host_info(&self.info) }
}

/// Initialize a HostDevice
pub async fn open_host_fidl(device_path: PathBuf) -> Result<FidlHost, Error> {
    // Connect to the host device.
    let host = File::open(device_path.clone())
                .map_err(|_| err_msg("failed to open bt-host device"))?;
    let handle = open_host_channel(&host)?;
    let handle = fasync::Channel::from_channel(handle.into())?;
    let proxy = HostProxy::new(handle);

    // Obtain basic information and create and entry in the disptacher's map.
    let info = await!(proxy.get_info())
        .map_err(|_| err_msg("failed to obtain bt-host information"))?;
    Ok(FidlHost{ device_path, proxy, info })
}

// TODO - This should perhaps be processed at the dispatcher level? Only the first requires
// modifying the Host, and that could be done by the dispatcher?
// TODO - OR, we could just forward everything that isn't an AdapterStateChanged?
//pub async fn run_host(event: HostEvent, dispatcher: HostDispatcher) -> fidl::Result<()> {
    //dispatcher.send(HostMsg(event))
//}

impl Discovery for FidlHost {
    fn start_discovery(&mut self) -> Receiver<bt::Result<()>> {
        let (sender, receiver) = oneshot::channel::<bt::Result<()>>();
        // TODO - what happens if send fails?
        fasync::spawn(self.proxy.start_discovery().map(|r| {let _todo = sender.send(r.map_err(fidl_err_to_bt).map(|_| ()));}));
        receiver
    }
    fn stop_discovery(&self) -> Receiver<bt::Result<()>> {
        let (sender, receiver) = oneshot::channel::<bt::Result<()>>();
        // TODO - what happens if send fails?
        fasync::spawn(self.proxy.stop_discovery().map(|r| { let _todo = sender.send(r.map_err(fidl_err_to_bt).map(|_| ()));}));
        receiver
    }
}

fn fidl_err_to_bt(_err: fidl::Error) -> bt::Error {
  bt::Error::BadResponseFromAdapter
}

impl Host for FidlHost {
    fn path(&self) -> &Path { &self.device_path }
}

#[cfg(test)]
mod fake {
    /// A Fake Host for unit testing
    pub struct Host {}

    impl Host {
        pub fn new() -> Host { Host {} }
    }
}

#[cfg(test)]
impl Discovery for fake::Host {
    fn start_discovery(&mut self) -> Receiver<bt::Result<()>> {
        let (sender,receiver) = oneshot::channel::<bt::Result<()>>();
        let _ = sender.send(Ok(()));
        receiver
    }
    fn stop_discovery(&self) -> Receiver<bt::Result<()>> {
        let (sender,receiver) = oneshot::channel::<bt::Result<()>>();
        let _ = sender.send(Ok(()));
        receiver
    }
}

#[cfg(test)]
impl Host for fake::Host {
    fn path(&self) -> &Path {
        panic!("NYI");
    }
}







/*
impl FidlHost {
    // TODO - what is the right API for creation?
    pub async fn open(device_path: PathBuf) -> Result<Self> {}

    pub fn path(&self) -> &Path { &self.device_path }

    pub fn get_info(&self) -> &AdapterInfo { &self.info }

    pub fn set_host_pairing_delegate(
        &self,
        input: InputCapabilityType,
        output: OutputCapabilityType,
        delegate: ClientEnd<PairingDelegateMarker>
    ) {
        self.proxy.set_pairing_delegate(input, output, Some(delegate));
    }

    pub fn connect(remote_device: DeviceId) {
        self.proxy.connect(remote_device)
    }
    pub fn disconnect(remote_device: DeviceId) {
        self.proxy.disconnect(remote_device)
    }

    pub async fn set_name(&self, mut name: String) -> bt::Result<()> {
        self.proxy.set_local_name(&mut name)
    }

    pub fn close(&self) -> bt::Result<()> {
        self.proxy.close()
    }

    pub async fn restore_bonds(&self, mut bonds: Vec<BondingData>) -> bt::Result<()> {
        self.proxy.add_bonded_devices(&mut bonds.iter_mut())
    }

    pub async fn set_connectable(&self, value: bool) -> bt::Result<()> {
        self.proxy.set_connectable(value)
    }

    pub async fn set_discoverable(&self, discoverable: bool) -> bt::Result<()> {
        self.proxy.set_discoverable(discoverable)
    }

    pub fn enable_background_scan(&self, enable: bool) -> bt::Result<()> {
        self.proxy.enable_background_scan(enable)
    }
}
    */

/*
trait HostLike {
    pub fn set_host_pairing_delegate( &self,
        input: InputCapabilityType, output: OutputCapabilityType,
        delegate: ClientEnd<PairingDelegateMarker>
    );

    pub fn get_info(&self) -> &AdapterInfo;

    pub fn start_discovery(&mut self) -> Receiver<bt::Result<()>>;
    pub fn stop_discovery(&self) -> Receiver<bt::Result<()>>;

    pub fn connect(remote_device: DeviceId);
    pub fn disconnect(remote_device: DeviceId);

    pub fn set_name(&self, mut name: String) -> Receiver<bt::Result<()>>;
    pub fn close(&self) -> bt::Result<()>;
    pub fn restore_bonds(&self, mut bonds: Vec<BondingData>) -> Receiver<bt::Result<()>>;
    pub fn set_connectable(&self, value: bool) -> Receiver<bt::Result<()>>;

    pub fn set_discoverable(&self, discoverable: bool) -> Receiver<bt::Result<()>>;
    pub fn enable_background_scan(&self, enable: bool) -> bt::Result<()>;
}
*/

trait Connectable {
    fn connect(remote_device: DeviceId);
    fn disconnect(remote_device: DeviceId);
}


