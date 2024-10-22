// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use crate::prelude::*;
use crate::spinel::*;
use futures::prelude::*;

use anyhow::Error;
use fasync::Time;
use lowpan_driver_common::{FutureExt as _, ZxResult};
use spinel_pack::TryOwnedUnpack;

/// Miscellaneous private methods
impl<DS: SpinelDeviceClient> SpinelDriver<DS> {
    /// This method is called whenever it is observed that the
    /// NCP is acting in a weird or spurious manner. This could
    /// be due to timeouts or bad byte packing, for example.
    pub(super) fn ncp_is_misbehaving(&self) {
        fx_log_err!("NCP is misbehaving.");

        // TODO: Add a counter?

        self.driver_state.lock().prepare_for_init();
        self.driver_state_change.trigger();
    }

    /// Decorates the given future with error mapping,
    /// reset handling, and a standard timeout.
    pub(super) fn apply_standard_combinators<'a, F>(
        &'a self,
        future: F,
    ) -> impl Future<Output = ZxResult<F::Ok>> + 'a
    where
        F: TryFuture<Error = Error> + Unpin + Send + 'a,
        <F as TryFuture>::Ok: Send,
    {
        future
            .map_err(|e| ZxStatus::from(ErrorAdapter(e)))
            .cancel_upon(self.ncp_did_reset.wait(), Err(ZxStatus::CANCELED))
            .on_timeout(Time::after(DEFAULT_TIMEOUT), ncp_cmd_timeout!(self))
    }

    /// Returns a future that gets a property and returns the value.
    pub(super) fn get_property_simple<T: TryOwnedUnpack + 'static, P: Into<Prop>>(
        &self,
        prop: P,
    ) -> impl Future<Output = ZxResult<T::Unpacked>> + '_ {
        self.apply_standard_combinators(
            self.frame_handler.send_request(CmdPropValueGet(prop.into()).returning::<T>()).boxed(),
        )
    }
}

/// State synchronization
impl<DS: SpinelDeviceClient> SpinelDriver<DS> {
    /// Handler for keeping track of property value changes
    /// so that local state stays in sync with the device.
    pub(super) fn on_prop_value_is(&self, prop: Prop, value: &[u8]) -> Result<(), Error> {
        fx_log_info!("on_prop_value_is: {:?} {:?}", prop, value);
        Ok(())
    }

    /// Handler for keeping track of property value insertions
    /// so that local state stays in sync with the device.
    pub(super) fn on_prop_value_inserted(&self, prop: Prop, value: &[u8]) -> Result<(), Error> {
        fx_log_info!("on_prop_value_inserted: {:?} {:?}", prop, value);
        Ok(())
    }

    /// Handler for keeping track of property value removals
    /// so that local state stays in sync with the device.
    pub(super) fn on_prop_value_removed(&self, prop: Prop, value: &[u8]) -> Result<(), Error> {
        fx_log_info!("on_prop_value_removed: {:?} {:?}", prop, value);
        Ok(())
    }
}
