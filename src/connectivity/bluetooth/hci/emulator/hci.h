// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_HCI_EMULATOR_HCI_H_
#define SRC_CONNECTIVITY_BLUETOOTH_HCI_EMULATOR_HCI_H_

#include <unordered_map>

#include "src/connectivity/bluetooth/core/bt-host/testing/fake_controller.h"
#include "src/connectivity/bluetooth/hci/emulator/peer.h"
#include "src/connectivity/bluetooth/lib/fidl/hanging_getter.h"

namespace bt_hci_emulator {

  class EmulatedHci {
    public:
      // Helper function used to initialize BR/EDR and LE peers.
      void AddPeer(std::unique_ptr<Peer> peer);

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


    private:
    // All objects below are only accessed on the |loop_| dispatcher.
    fbl::RefPtr<bt::testing::FakeController> controller_;

    // List of active peers that have been registered with us.
    std::unordered_map<bt::DeviceAddress, std::unique_ptr<Peer>> peers_;

    bt_lib_fidl::HangingVectorGetter<fuchsia::bluetooth::test::LegacyAdvertisingState>
      legacy_adv_state_getter_;
  };

}

#endif  // SRC_CONNECTIVITY_BLUETOOTH_HCI_EMULATOR_HCI_H_
