// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(nickpollard) fidl service

namespace bt_hci_emulator {

  // fuchsia.bluetooth.test.Emulator fidl Service server
  // Handles fidl requests and directs to the emulator internals
  class EmulatorService : public fuchsia::bluetooth::test::HciEmulator {

    // fuchsia::bluetooth::test::HciEmulator overrides:
    void Publish(fuchsia::bluetooth::test::EmulatorSettings settings,
        PublishCallback callback) override;

    void AddLowEnergyPeer(fuchsia::bluetooth::test::LowEnergyPeerParameters params,
        fidl::InterfaceRequest<fuchsia::bluetooth::test::Peer> request,
        AddLowEnergyPeerCallback callback) override;
    void AddBredrPeer(fuchsia::bluetooth::test::BredrPeerParameters params,
        fidl::InterfaceRequest<fuchsia::bluetooth::test::Peer> request,
        AddBredrPeerCallback callback) override;

    void WatchLeScanStates(WatchLeScanStatesCallback callback) override;
    void WatchLegacyAdvertisingStates(WatchLegacyAdvertisingStatesCallback callback) override;

    void OnLegacyAdvertisingStateChanged();

    void OnPeerConnectionStateChanged(const bt::DeviceAddress& address,
        bt::hci::ConnectionHandle handle, bool connected,
        bool canceled);
  }
}
