// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hci_device.h"

// NOTE: We do not implement unbind and release. The lifecycle of the bt-hci
// device is strictly tied to the bt-emulator device (i.e. it can never out-live
// bt-emulator). We handle its destruction in the bt_emulator_device_ops
// messages.
static zx_protocol_device_t bt_hci_device_ops = {
    .version = DEVICE_OPS_VERSION,
    .get_protocol = [](void* ctx, uint32_t proto_id, void* out_proto) -> zx_status_t {
      return static_cast<HciDevice*>(ctx)->GetProtocol(proto_id, out_proto);
    },
    .message = [](void* ctx, fidl_msg_t* msg, fidl_txn_t* txn) -> zx_status_t {
      return static_cast<HciDevice*>(ctx)->HciMessage(msg, txn);
    }
};


// Attempt to create and publish the child HCI device
static zx_status_t HciDevice::PublishDevice(zx_device_t* parent, FakeController::Settings settings) {
  logf(TRACE, "HciEmulator.Publish\n");

  auto device = std::make_unique<HciDevice>(settings);

  // Publish the bt-hci device.
  device_add_args_t args = {
      .version = DEVICE_ADD_ARGS_VERSION,
      .name = "bt_hci_emulator",
      .ctx = device,
      .ops = &bt_hci_device_ops,
      .proto_id = ZX_PROTOCOL_BT_HCI,
  };
  device_add(parent, &args, &device.hci_dev_);
  if (status == ZX_OK) {
    // TODO(nickpollard) - is this correct?
    // The DDK owns the lifetime of this now
    device.release()
  }
  return status;
}

// Return the HCI banjo protocol
zx_status_t HciDevice::GetProtocol(uint32_t proto_id, void* out_proto) {
  // The bt-emulator HCI device doesn't support a non-FIDL protocol.
  if (proto_id != ZX_PROTOCOL_BT_HCI) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  bt_hci_protocol_t* hci_proto = static_cast<bt_hci_protocol_t*>(out_proto);
  hci_proto->ctx = this;
  hci_proto->ops = &hci_protocol_ops;

  return ZX_OK;
}

// Handle an Hci banjo(?) message from the DDK
zx_status_t HciDevice::HciMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  logf(TRACE, "HciMessage\n");
  return fuchsia_hardware_bluetooth_Hci_dispatch(this, txn, msg, &hci_fidl_ops_);
}

static constexpr fuchsia_hardware_bluetooth_Hci_ops_t hci_fidl_ops_ = {
  .OpenCommandChannel = OpenCommandChannel,
  .OpenAclDataChannel = OpenAclDataChannel,
  .OpenSnoopChannel = OpenSnoopChannel,
};

static bt_hci_protocol_ops_t hci_protocol_ops = {
  .open_command_channel = HciDevice::OpenCommandChannel,
  .open_acl_data_channel = HciDevice::OpenAclDataChannel,
  .open_snoop_channel = HciDevice::OpenSnoopChannel,
  //.open_command_channel = [](void* ctx, zx_handle_t chan) -> zx_status_t {
  //return DEV(ctx)->OpenChan(Channel::COMMAND, chan);
  //},
  //.open_acl_data_channel = [](void* ctx, zx_handle_t chan) -> zx_status_t {
  //return DEV(ctx)->OpenChan(Channel::ACL, chan);
  //},
  //.open_snoop_channel = [](void* ctx, zx_handle_t chan) -> zx_status_t {
  //return DEV(ctx)->OpenChan(Channel::SNOOP, chan);
  //},
};

zx_status_t EmulatorDevice::OpenCommandChannel(void* ctx, zx_handle_t channel) {
  return static_cast<Device*>(ctx)->OpenChan(Channel::COMMAND, channel);
}

zx_status_t EmulatorDevice::OpenAclDataChannel(void* ctx, zx_handle_t channel) {
  return static_cast<Device*>(ctx)->OpenChan(Channel::ACL, channel);
}

zx_status_t EmulatorDevice::OpenSnoopChannel(void* ctx, zx_handle_t channel) {
  return static_cast<Device*>(ctx)->OpenChan(Channel::SNOOP, channel);
}

// TODO(nickpollard) - couldn't this be inlined?
zx_status_t EmulatorDevice::OpenChan(Channel chan_type, zx_handle_t in_h) {
  logf(TRACE, "open HCI channel\n");

  zx::channel in(in_h);

  if (chan_type == Channel::COMMAND) {
    async::PostTask(loop_.dispatcher(), [controller = fake_controller_, in = std::move(in)]() mutable {
      controller->StartCmdChannel(std::move(in));
    });
  } else if (chan_type == Channel::ACL) {
    async::PostTask(loop_.dispatcher(), [controller = fake_controller_, in = std::move(in)]() mutable {
      controller->StartAclChannel(std::move(in));
    });
  } else if (chan_type == Channel::SNOOP) {
    async::PostTask(loop_.dispatcher(), [controller = fake_controller_, in = std::move(in)]() mutable {
      controller->StartSnoopChannel(std::move(in));
    });
  } else {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}
