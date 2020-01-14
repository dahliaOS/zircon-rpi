// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <lib/async/cpp/task.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <cstdio>
#include <future>
#include <thread>

#include <ddk/protocol/bt/hci.h>

#include "src/connectivity/bluetooth/core/bt-host/testing/fake_peer.h"
#include "src/connectivity/bluetooth/hci/emulator/log.h"

namespace fbt = fuchsia::bluetooth;
namespace ftest = fuchsia::bluetooth::test;

using bt::DeviceAddress;
using bt::testing::FakeController;
using bt::testing::FakePeer;

namespace bt_hci_emulator {

// The standard Zircon DDK device operations passed to device_add
#define DEV(c) static_cast<EmulatorDevice*>(c)
static zx_protocol_device_t bt_emulator_device_ops = {
    .version = DEVICE_OPS_VERSION,
    .get_protocol = [](void* ctx, uint32_t proto_id, void* out_proto) -> zx_status_t {
      return DEV(ctx)->GetProtocol(proto_id, out_proto);
    },
    .unbind = [](void* ctx) { DEV(ctx)->Unbind(); },
    .release = [](void* ctx) { DEV(ctx)->Release(); },
    .message = [](void* ctx, fidl_msg_t* msg,
                  fidl_txn_t* txn) { return DEV(ctx)->EmulatorMessage(msg, txn); }};
#undef DEV

EmulatorDevice::EmulatorDevice(zx_device_t* device)
    : loop_(&kAsyncLoopConfigNoAttachToCurrentThread),
      parent_(device),
      hci_dev_(std::nullopt),
      emulator_dev_(nullptr),
      binding_(this) {}

// Attempt to create and bind an EmulatorDevice
zx_status_t EmulatorDevice::Bind() {
  logf(TRACE, "bind\n");

  auto device = std::make_unique<bt_hci_emulator::Device>(device);

  // We add the device as invisible so that it cannot be interfaced with until initialization is
  // complete, e.g. once we have started the loop_ thread. This ensures that the device `unbind()`
  // operation is not called before this function returns. This guarantees ordering between `bind()`
  // and `unbind()` and prevents race conditions between objects (such as `loop_`) that are
  // initialized here and deinitialized in `unbind()`.
  device_add_args_t args = {
      .version = DEVICE_ADD_ARGS_VERSION,
      .name = "bt_hci_emulator",
      .ctx = device,
      .ops = &bt_emulator_device_ops,
      .proto_id = ZX_PROTOCOL_BT_EMULATOR,
      .flags = DEVICE_ADD_NON_BINDABLE | DEVICE_ADD_INVISIBLE,
  };
  auto status = device_add(parent_, &args, &emulator_dev_);
  if (status != ZX_OK) {
    logf(ERROR, "could not add bt-emulator device: %s\n", zx_status_get_string(status));
    return status;
  }

  /*
  fake_device_ = fbl::AdoptRef(new FakeController());
  fake_device_->set_controller_parameters_callback(
      fit::bind_member(this, &Device::OnControllerParametersChanged));
  fake_device_->set_advertising_state_callback(
      fit::bind_member(this, &Device::OnLegacyAdvertisingStateChanged));
  fake_device_->set_connection_state_callback(
      fit::bind_member(this, &Device::OnPeerConnectionStateChanged));
      */
  // If bound, the DDK now manages the lifetime of the object we passed
  device.release();

  loop_.StartThread("bt_hci_emulator");

  // The device is now fully intialized, so it is safe to make it visible
  device_make_visible(emulator_dev_);

  return status;
}

void EmulatorDevice::Release() {
  logf(TRACE, "release\n");
  // TODO(nickpollard) - this is not safe due to race conditions
  // we need to wait to ensure that nothing else is running, and ensure they dont run after
  delete this;
}

void EmulatorDevice::Unbind() {
  logf(TRACE, "unbind\n");

  // Clean up all FIDL channels and the underlying FakeController on the
  // dispatcher thread, due to the FakeController object's thread-safety
  // requirements. It is OK to capture references to members in the task since
  // this function will block until the dispatcher loop has terminated.
  async::PostTask(loop_.dispatcher(), [binding = &binding_, loop = &loop_] {
    binding->Unbind();
    loop->Quit();
  });

  // Block here until all the shutdown tasks we just posted are completed on the FIDL/emulator
  // dispatcher thread to guarantee that the operations below don't happen concurrently with them.
  loop_.JoinThreads();
  logf(TRACE, "emulator dispatcher shut down\n");

  // Clean up all fake peers. This will close their local channels and remove them from the fake
  // controller.
  peers_.clear();

  UnpublishHci();

  device_async_remove(emulator_dev_);
  emulator_dev_ = nullptr;
}

zx_status_t EmulatorDevice::EmulatorMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  logf(TRACE, "EmulatorMessage\n");
  return fuchsia_hardware_bluetooth_Emulator_dispatch(this, txn, msg, &emul_fidl_ops_);
}

zx_status_t EmulatorDevice::GetProtocol(uint32_t proto_id, void* out_proto) {
  // The emulator itself has no protocol
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t EmulatorDevice::OpenChan(Channel chan_type, zx_handle_t in_h) {
  logf(TRACE, "open HCI channel\n");

  zx::channel in(in_h);

  if (chan_type == Channel::EMULATOR) {
    async::PostTask(loop_.dispatcher(), [this, in = std::move(in)]() mutable {
      StartEmulatorInterface(std::move(in));
    });
  } else {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

void EmulatorDevice::StartEmulatorInterface(zx::channel chan) {
  logf(TRACE, "start HciEmulator interface\n");

  if (binding_.is_bound()) {
    logf(TRACE, "HciEmulator channel already bound\n");
    return;
  }

  // Process HciEmulator messages on a thread that can safely access the
  // FakeController, which is thread-hostile.
  binding_.Bind(std::move(chan), loop_.dispatcher());
  binding_.set_error_handler([this](zx_status_t status) {
    logf(TRACE, "emulator channel closed (status: %s); unpublish device\n",
         zx_status_get_string(status));
    UnpublishHci();
  });
}

// TODO(nickpollard) - trim this down
// Publish the child HCI device
void EmulatorDevice::Publish(ftest::EmulatorSettings in_settings, PublishCallback callback) {
  logf(TRACE, "HciEmulator.Publish\n");

  ftest::HciEmulator_Publish_Result result;
  if (hci_dev_) {
    result.set_err(ftest::EmulatorError::HCI_ALREADY_PUBLISHED);
    callback(std::move(result));
    return;
  }

  FakeController::Settings config = SettingsFromFidl(in_settings);
  // TODO(nickpollard) - store the HciDevice if it succeeds
  auto status = HciDevice::PublishDevice(config);

  if (status != ZX_OK) {
    result.set_err(ftest::EmulatorError::FAILED);
  } else {
    result.set_response(ftest::HciEmulator_Publish_Response{});
  }

  callback(std::move(result));
}

/*
void Device::AddLowEnergyPeer(ftest::LowEnergyPeerParameters params,
                              fidl::InterfaceRequest<ftest::Peer> request,
                              AddLowEnergyPeerCallback callback) {
  logf(TRACE, "HciEmulator.AddLowEnergyPeer\n");

  ftest::HciEmulator_AddLowEnergyPeer_Result fidl_result;

  auto result = Peer::NewLowEnergy(std::move(params), std::move(request), fake_device_);
  if (result.is_error()) {
    fidl_result.set_err(result.error());
    callback(std::move(fidl_result));
    return;
  }

  AddPeer(result.take_value());
  fidl_result.set_response(ftest::HciEmulator_AddLowEnergyPeer_Response{});
  callback(std::move(fidl_result));
}

void Device::AddBredrPeer(ftest::BredrPeerParameters params,
                          fidl::InterfaceRequest<fuchsia::bluetooth::test::Peer> request,
                          AddBredrPeerCallback callback) {
  logf(TRACE, "HciEmulator.AddBredrPeer\n");

  ftest::HciEmulator_AddBredrPeer_Result fidl_result;

  auto result = Peer::NewBredr(std::move(params), std::move(request), fake_device_);
  if (result.is_error()) {
    fidl_result.set_err(result.error());
    callback(std::move(fidl_result));
    return;
  }

  AddPeer(result.take_value());
  fidl_result.set_response(ftest::HciEmulator_AddBredrPeer_Response{});
  callback(std::move(fidl_result));
}

void Device::WatchControllerParameters(WatchControllerParametersCallback callback) {
  logf(TRACE, "HciEmulator.WatchControllerParameters\n");
  controller_parameters_getter_.Watch(std::move(callback));
}

void Device::WatchLeScanStates(WatchLeScanStatesCallback callback) {
  // TODO(BT-229): Implement
}

void Device::WatchLegacyAdvertisingStates(WatchLegacyAdvertisingStatesCallback callback) {
  logf(TRACE, "HciEmulator.WatchLegacyAdvertisingState\n");
  legacy_adv_state_getter_.Watch(std::move(callback));
}

void Device::AddPeer(std::unique_ptr<Peer> peer) {
  auto address = peer->address();
  peer->set_closed_callback([this, address] { peers_.erase(address); });
  peers_[address] = std::move(peer);
}

void Device::OnControllerParametersChanged() {
  logf(TRACE, "HciEmulator.OnControllerParametersChanged\n");

  ftest::ControllerParameters fidl_value;
  fidl_value.set_local_name(fake_device_->local_name());

  const auto& device_class_bytes = fake_device_->device_class().bytes();
  uint32_t device_class = 0;
  device_class |= device_class_bytes[0];
  device_class |= static_cast<uint32_t>(device_class_bytes[1]) << 8;
  device_class |= static_cast<uint32_t>(device_class_bytes[2]) << 16;
  fidl_value.set_device_class(fbt::DeviceClass{device_class});

  controller_parameters_getter_.Set(std::move(fidl_value));
}

void Device::OnLegacyAdvertisingStateChanged() {
  logf(TRACE, "HciEmulator.OnLegacyAdvertisingStateChanged\n");

  // We have requests to resolve. Construct the FIDL table for the current state.
  ftest::LegacyAdvertisingState fidl_state;
  FakeController::LEAdvertisingState adv_state = fake_device_->le_advertising_state();
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

void Device::OnPeerConnectionStateChanged(const bt::DeviceAddress& address,
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
*/

void EmulatorDevice::UnpublishHci() {
  hci_dev_.UnpublishDevice();
}

zx_status_t EmulatorDevice::OpenEmulatorChannel(void* ctx, zx_handle_t channel) {
  return static_cast<Device*>(ctx)->OpenChan(Channel::EMULATOR, channel);
}

}  // namespace bt_hci_emulator
