// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_HCI_EMULATOR_DEVICE_H_
#define SRC_CONNECTIVITY_BLUETOOTH_HCI_EMULATOR_DEVICE_H_

#include <fuchsia/bluetooth/test/cpp/fidl.h>
#include <fuchsia/hardware/bluetooth/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding.h>
#include <zircon/compiler.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <queue>
#include <unordered_map>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/bt/hci.h>
#include <ddk/protocol/test.h>

#include "src/connectivity/bluetooth/core/bt-host/testing/fake_controller.h"
#include "src/connectivity/bluetooth/hci/emulator/peer.h"
#include "src/connectivity/bluetooth/lib/fidl/hanging_getter.h"

namespace bt_hci_emulator {

enum class Channel {
  ACL,
  COMMAND,
  SNOOP,
  EMULATOR,
};

class EmulatorDevice : public fuchsia::bluetooth::test::HciEmulator {
 public:
  explicit EmulatorDevice(zx_device_t* device);

  // DDK lifecycle driver bind
  static zx_status_t Bind_();

  // DDK lifecycle device unbind
  void Unbind();
  // DDK lifecycle device release
  void Release();

  zx_status_t EmulatorMessage(fidl_msg_t* msg, fidl_txn_t* txn);
  zx_status_t GetProtocol(uint32_t proto_id, void* out_proto);
  zx_status_t OpenChan(Channel chan_type, zx_handle_t chan);

  static zx_status_t OpenEmulatorChannel(void* ctx, zx_handle_t channel);

 private:
  void StartEmulatorInterface(zx::channel chan);

  /*
  // fuchsia::bluetooth::test::HciEmulator overrides:
  void Publish(fuchsia::bluetooth::test::EmulatorSettings settings,
               PublishCallback callback) override;
  void AddLowEnergyPeer(fuchsia::bluetooth::test::LowEnergyPeerParameters params,
                        fidl::InterfaceRequest<fuchsia::bluetooth::test::Peer> request,
                        AddLowEnergyPeerCallback callback) override;
  void AddBredrPeer(fuchsia::bluetooth::test::BredrPeerParameters params,
                    fidl::InterfaceRequest<fuchsia::bluetooth::test::Peer> request,
                    AddBredrPeerCallback callback) override;
  void WatchControllerParameters(WatchControllerParametersCallback callback) override;
  void WatchLeScanStates(WatchLeScanStatesCallback callback) override;
  void WatchLegacyAdvertisingStates(WatchLegacyAdvertisingStatesCallback callback) override;

  // Helper function used to initialize BR/EDR and LE peers.
  void AddPeer(std::unique_ptr<Peer> peer);

  void OnControllerParametersChanged();
  void OnLegacyAdvertisingStateChanged();
  */

  // Remove the bt-hci device.
  void UnpublishHci();

  static constexpr fuchsia_hardware_bluetooth_Emulator_ops_t emul_fidl_ops_ = {
      .Open = OpenEmulatorChannel,
  };

  // Dispatch loop to run all non-DDK tasks on
  // The only code that does not run on this loop is direct calls from the DDK
  async::Loop loop_;

  // Parent device
  zx_device_t* const parent_;

  // The device that implements the bt-emulator protocol.
  zx_device_t* emulator_dev_;

  // Binding for fuchsia.bluetooth.test.HciEmulator channel. |binding_| is only accessed on
  // |loop_|'s dispatcher.
  fidl::Binding<fuchsia::bluetooth::test::HciEmulator> binding_;

  // The device that implements the bt-hci protocol. |hci_dev_| will only be accessed and modified
  // on the following threads/conditions:
  //   1. It only gets initialized on the |loop_| dispatcher during Publish().
  //   2. Unpublished when the HciEmulator FIDL channel (i.e. |binding_|) gets closed, which gets
  //      processed on the |loop_| dispatcher.
  //   3. Unpublished in the DDK Unbind() call which runs on a devhost thread. This is guaranteed
  //      not to happen concurrently with #1 and #2 as Unbind always drains and joins the |loop_|
  //      threads before removing devices.
  std::option<HciDevice> hci_dev_;

  /*
  bt_lib_fidl::HangingGetter<fuchsia::bluetooth::test::ControllerParameters>
      controller_parameters_getter_;
  bt_lib_fidl::HangingVectorGetter<fuchsia::bluetooth::test::LegacyAdvertisingState>
      legacy_adv_state_getter_;
      */

  std::option<EmulatedHci> hci_;
};

}  // namespace bt_hci_emulator

#endif  // SRC_CONNECTIVITY_BLUETOOTH_HCI_EMULATOR_DEVICE_H_
