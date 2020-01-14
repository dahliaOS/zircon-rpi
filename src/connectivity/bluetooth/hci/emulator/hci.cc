// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

EmulatedHci::EmulatedHci(FakeController::Settings settings) {
  controller_->set_settings(settings);
}

void EmulatedHci::AddPeer(std::unique_ptr<Peer> peer) {
  auto address = peer->address();
  peer->set_closed_callback([this, address] { peers_.erase(address); });
  peers_[address] = std::move(peer);
}

void EmulatedHci::AddLowEnergyPeer(ftest::LowEnergyPeerParameters params,
                              fidl::InterfaceRequest<ftest::Peer> request,
                              AddLowEnergyPeerCallback callback) {
  logf(TRACE, "HciEmulator.AddLowEnergyPeer\n");

  ftest::HciEmulator_AddLowEnergyPeer_Result fidl_result;

  auto result = Peer::NewLowEnergy(std::move(params), std::move(request), controller_);
  if (result.is_error()) {
    fidl_result.set_err(result.error());
    callback(std::move(fidl_result));
    return;
  }

  hci_.AddPeer(result.take_value());
  fidl_result.set_response(ftest::HciEmulator_AddLowEnergyPeer_Response{});
  callback(std::move(fidl_result));
}

void EmulatedHci::AddBredrPeer(ftest::BredrPeerParameters params,
                          fidl::InterfaceRequest<fuchsia::bluetooth::test::Peer> request,
                          AddBredrPeerCallback callback) {
  logf(TRACE, "HciEmulator.AddBredrPeer\n");

  ftest::HciEmulator_AddBredrPeer_Result fidl_result;

  auto result = Peer::NewBredr(std::move(params), std::move(request), controller_);
  if (result.is_error()) {
    fidl_result.set_err(result.error());
    callback(std::move(fidl_result));
    return;
  }

  hci_.AddPeer(result.take_value());
  fidl_result.set_response(ftest::HciEmulator_AddBredrPeer_Response{});
  callback(std::move(fidl_result));
}

void EmulatedHci::WatchLeScanStates(WatchLeScanStatesCallback callback) {
  // TODO(BT-229): Implement
}

void EmulatedHci::WatchLegacyAdvertisingStates(WatchLegacyAdvertisingStatesCallback callback) {
  logf(TRACE, "HciEmulator.WatchLegacyAdvertisingState\n");
  if (!legacy_adv_state_getter_.Watch(std::move(callback))) {
    binding_.Unbind();
    UnpublishHci();
  }
}

void EmulatedHci::OnLegacyAdvertisingStateChanged() {
  logf(TRACE, "HciEmulator.OnLegacyAdvertisingStateChanged\n");

  // We have requests to resolve. Construct the FIDL table for the current state.
  ftest::LegacyAdvertisingState fidl_state;
  FakeController::LEAdvertisingState adv_state = controller_->le_advertising_state();
  fidl_state.set_enabled(adv_state.enabled);

  // Populate the rest only if advertising is enabled.
  fidl_state.set_type(static_cast<ftest::LegacyAdvertisingType>(adv_state.adv_type));
  fidl_state.set_address_type(LeOwnAddressTypeToFidl(adv_state.own_address_type));

  if (adv_state.interval_min) {
    fidl_state.set_interval_min(adv_state.interval_min);
  }
  if (adv_state.interval_max) {
    fidl_state.set_interval_max(adv_state.interval_max);
  }

  if (adv_state.data_length) {
    std::vector<uint8_t> output(adv_state.data_length);
    bt::MutableBufferView output_view(output.data(), output.size());
    output_view.Write(adv_state.data, adv_state.data_length);
    fidl_state.set_advertising_data(std::move(output));
  }
  if (adv_state.scan_rsp_length) {
    std::vector<uint8_t> output(adv_state.scan_rsp_length);
    bt::MutableBufferView output_view(output.data(), output.size());
    output_view.Write(adv_state.scan_rsp_data, adv_state.scan_rsp_length);
    fidl_state.set_scan_response(std::move(output));
  }

  legacy_adv_state_getter_.Add(std::move(fidl_state));
}

void EmulatedHci::OnPeerConnectionStateChanged(const bt::DeviceAddress& address,
                                          bt::hci::ConnectionHandle handle, bool connected,
                                          bool canceled) {
  logf(TRACE, "Peer connection state changed: %s (handle: %#.4x) (connected: %s) (canceled: %s):\n",
       address.ToString().c_str(), handle, (connected ? "true" : "false"),
       (canceled ? "true" : "false"));

  auto iter = peers_.find(address);
  if (iter != peers_.end()) {
    iter->second->UpdateConnectionState(connected);
  }
}

void EmulatedHci::Stop() {
  controller_->Stop();

  // Clean up all fake peers. This will close their local channels and remove them from the fake
  // controller.
  peers_.clear();

  // Destroy the FakeController here. Since |loop_| has been shutdown, we
  // don't expect it to be dereferenced again.
  controller_ = nullptr;
  UnpublishHci();
}

