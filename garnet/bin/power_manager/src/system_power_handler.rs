// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(fxbug.dev/54210): This node will be obsolete once the new power_manager based shutdown path
// is in place. Delete this node once the new shutdown path is active.

use crate::error::PowerManagerError;
use crate::log_if_err;
use crate::message::{Message, MessageReturn};
use crate::node::Node;
use crate::shutdown_request::ShutdownRequest;
use crate::utils::connect_proxy;
use anyhow::{format_err, Error};
use async_trait::async_trait;
use fidl_fuchsia_device_manager as fdevmgr;
use fuchsia_inspect::{self as inspect, NumericProperty, Property};
use fuchsia_zircon as zx;
use log::*;
use serde_json as json;
use std::collections::HashMap;
use std::rc::Rc;

/// Node: SystemPowerStateHandler
///
/// Summary: Interacts with the device manager service to command system power state
///          changes (i.e., reboot, shutdown, suspend-to-ram)
///
/// Handles Messages:
///     - SystemShutdown
///
/// Sends Messages: N/A
///
/// FIDL dependencies:
///     - fuchsia.device.manager.Administrator: the node connects to this service to command system
///       power state changes

/// The device manager service that we'll be communicating with
const DEV_MGR_SVC: &'static str = "/svc/fuchsia.device.manager.Administrator";

/// A builder for constructing the SystemPowerStateHandler node
pub struct SystemPowerStateHandlerBuilder<'a> {
    svc_proxy: Option<fdevmgr::AdministratorProxy>,
    inspect_root: Option<&'a inspect::Node>,
}

impl<'a> SystemPowerStateHandlerBuilder<'a> {
    pub fn new() -> Self {
        Self { svc_proxy: None, inspect_root: None }
    }

    pub fn new_from_json(_json_data: json::Value, _nodes: &HashMap<String, Rc<dyn Node>>) -> Self {
        Self::new()
    }

    #[cfg(test)]
    pub fn with_proxy(mut self, proxy: fdevmgr::AdministratorProxy) -> Self {
        self.svc_proxy = Some(proxy);
        self
    }

    #[cfg(test)]
    pub fn with_inspect_root(mut self, root: &'a inspect::Node) -> Self {
        self.inspect_root = Some(root);
        self
    }

    pub fn build(self) -> Result<Rc<SystemPowerStateHandler>, Error> {
        // Optionally use the default proxy
        let proxy = if self.svc_proxy.is_none() {
            connect_proxy::<fdevmgr::AdministratorMarker>(&DEV_MGR_SVC.to_string())?
        } else {
            self.svc_proxy.unwrap()
        };

        // Optionally use the default inspect root node
        let inspect_root = self.inspect_root.unwrap_or(inspect::component::inspector().root());

        Ok(Rc::new(SystemPowerStateHandler {
            svc_proxy: proxy,
            inspect: InspectData::new(inspect_root, "SystemPowerStateHandler".to_string()),
        }))
    }
}

pub struct SystemPowerStateHandler {
    svc_proxy: fdevmgr::AdministratorProxy,

    /// A struct for managing Component Inspection data
    inspect: InspectData,
}

impl SystemPowerStateHandler {
    async fn handle_system_shutdown(
        &self,
        request: &ShutdownRequest,
    ) -> Result<MessageReturn, PowerManagerError> {
        fuchsia_trace::instant!(
            "power_manager",
            "SystemPowerStateHandler::handle_system_shutdown",
            fuchsia_trace::Scope::Thread,
            "request" => format!("{:?}", request).as_str()
        );
        info!("System shutdown ({:?})", request);
        let result = self.dev_mgr_suspend(request.to_driver_manager_flag()).await;
        log_if_err!(result, "System shutdown failed");
        fuchsia_trace::instant!(
            "power_manager",
            "SystemPowerStateHandler::dev_mgr_suspend_result",
            fuchsia_trace::Scope::Thread,
            "result" => format!("{:?}", result).as_str()
        );

        match result {
            Ok(_) => Ok(MessageReturn::SystemShutdown),
            Err(e) => {
                self.inspect.shutdown_errors.add(1);
                self.inspect.last_shutdown_error.set(format!("{}", e).as_str());
                Err(PowerManagerError::GenericError(e))
            }
        }
    }

    async fn dev_mgr_suspend(&self, suspend_flag: u32) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "SystemPowerStateHandler::dev_mgr_suspend",
            "suspend_flag" => suspend_flag
        );
        let status = self.svc_proxy.suspend(suspend_flag).await.map_err(|e| {
            format_err!("DeviceManager Suspend failed: flag: {}; error: {}", suspend_flag, e)
        })?;
        zx::Status::ok(status).map_err(|e| {
            format_err!(
                "DeviceManager Suspend returned error: flag: {}, error: {}",
                suspend_flag,
                e
            )
        })?;

        self.inspect.set_system_power_state(suspend_flag);
        Ok(())
    }
}

#[async_trait(?Send)]
impl Node for SystemPowerStateHandler {
    fn name(&self) -> &'static str {
        "SystemPowerStateHandler"
    }

    async fn handle_message(&self, msg: &Message) -> Result<MessageReturn, PowerManagerError> {
        match msg {
            Message::SystemShutdown(request) => self.handle_system_shutdown(request).await,
            _ => Err(PowerManagerError::Unsupported),
        }
    }
}

struct InspectData {
    system_power_state: inspect::StringProperty,
    shutdown_errors: inspect::UintProperty,
    last_shutdown_error: inspect::StringProperty,
}

impl InspectData {
    fn new(parent: &inspect::Node, name: String) -> Self {
        // Create a local root node and properties
        let root = parent.create_child(name);
        let system_power_state = root.create_string("system_power_state", "fully_on");
        let shutdown_errors = root.create_uint("shutdown_errors", 0);
        let last_shutdown_error = root.create_string("last_shutdown_error", "");

        // Pass ownership of the new node to the parent node, otherwise it'll be dropped
        parent.record(root);

        InspectData { system_power_state, shutdown_errors, last_shutdown_error }
    }

    fn set_system_power_state(&self, suspend_flag: u32) {
        self.system_power_state.set(match suspend_flag {
            fdevmgr::SUSPEND_FLAG_POWEROFF => "power_off",
            fdevmgr::SUSPEND_FLAG_REBOOT => "reboot",
            _ => "unknown",
        })
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use crate::shutdown_request::RebootReason;
    use fuchsia_async as fasync;
    use futures::TryStreamExt;
    use inspect::assert_inspect_tree;
    use std::cell::Cell;
    use std::rc::Rc;

    fn setup_fake_service(
        mut shutdown_function: impl FnMut() + 'static,
    ) -> fdevmgr::AdministratorProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<fdevmgr::AdministratorMarker>().unwrap();

        fasync::spawn_local(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(fdevmgr::AdministratorRequest::Suspend {
                        flags: fdevmgr::SUSPEND_FLAG_REBOOT,
                        responder,
                    }) => {
                        shutdown_function();
                        let _ = responder.send(zx::Status::OK.into_raw());
                    }
                    _ => assert!(false),
                }
            }
        });

        proxy
    }

    /// Creates a test SystemPowerStateHandler, for which the supplied `shutdown_function` will
    /// be called by the underlying FIDL endpoint when SystemShutdown is requested.
    pub fn setup_test_node(
        shutdown_function: impl FnMut() + 'static,
    ) -> Rc<SystemPowerStateHandler> {
        SystemPowerStateHandlerBuilder::new()
            .with_proxy(setup_fake_service(shutdown_function))
            .build()
            .unwrap()
    }

    /// Tests that an unsupported message is handled gracefully and an Unsupported error is returned
    #[fasync::run_singlethreaded(test)]
    async fn test_unsupported_msg() {
        let node = setup_test_node(|| {});
        match node.handle_message(&Message::ReadTemperature).await {
            Err(PowerManagerError::Unsupported) => {}
            e => panic!("Unexpected return value: {:?}", e),
        }
    }

    /// Tests that the node can handle the 'SystemShutdown' message as expected. The test node uses
    /// a fake device manager service here, so a system shutdown will not actually happen.
    #[fasync::run_singlethreaded(test)]
    async fn test_system_shutdown() {
        let shutdown_applied = Rc::new(Cell::new(false));
        let shutdown_applied_2 = shutdown_applied.clone();
        let node = setup_test_node(move || {
            shutdown_applied_2.set(true);
        });
        match node
            .handle_message(&Message::SystemShutdown(ShutdownRequest::Reboot(
                RebootReason::UserRequest,
            )))
            .await
            .unwrap()
        {
            MessageReturn::SystemShutdown => {}
            _ => assert!(false),
        }
        assert!(shutdown_applied.get());
    }

    /// Tests for the presence and correctness of dynamically-added inspect data
    #[fasync::run_singlethreaded(test)]
    async fn test_inspect_data() {
        let inspect = inspect::Inspector::new();
        let _node = SystemPowerStateHandlerBuilder::new()
            .with_proxy(fidl::endpoints::create_proxy::<fdevmgr::AdministratorMarker>().unwrap().0)
            .with_inspect_root(inspect.root())
            .build()
            .unwrap();

        assert_inspect_tree!(
            inspect,
            root: {
                "SystemPowerStateHandler": contains {}
            }
        );
    }

    /// Tests that well-formed configuration JSON does not panic the `new_from_json` function.
    #[fasync::run_singlethreaded(test)]
    async fn test_new_from_json() {
        let json_data = json::json!({
            "type": "SystemPowerHandler",
            "name": "sys_power"
        });
        let _ = SystemPowerStateHandlerBuilder::new_from_json(json_data, &HashMap::new());
    }
}
