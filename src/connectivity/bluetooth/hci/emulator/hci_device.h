// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/protocol/bt/hci.h>

namespace bt_hci_emulator {
  class HciDevice {
    public:
      // Publish the HCI device
      //TODO(nickpollard) - make this a smart constructor and use RAII?
      static zx_status_t PublishDevice(zx_device_t* parent, ftest::EmulatorSettings in_settings);

      ~HciDevice() {
        if (hci_dev_) {
          device_remove_deprecated(hci_dev_);
          hci_dev_ = nullptr;
        }
      }

      zx_status_t GetProtocol(uint32_t proto_id, void* out_proto);
      static zx_status_t OpenCommandChannel(void* ctx, zx_handle_t channel);
      static zx_status_t OpenAclDataChannel(void* ctx, zx_handle_t channel);
      static zx_status_t OpenSnoopChannel(void* ctx, zx_handle_t channel);

    private:
      HciDevice(FakeController::Settings settings) :
        hci_(settings)
      {}

      // The device that implements the bt-hci protocol. |hci_dev_| will only be accessed and modified
      // on the following threads/conditions:
      //   1. It only gets initialized on the |loop_| dispatcher during Publish().
      //   2. Unpublished when the HciEmulator FIDL channel (i.e. |binding_|) gets closed, which gets
      //      processed on the |loop_| dispatcher.
      //   3. Unpublished in the DDK Unbind() call which runs on a devhost thread. This is guaranteed
      //      not to happen concurrently with #1 and #2 as Unbind always drains and joins the |loop_|
      //      threads before removing devices.
      zx_device_t* hci_dev_;

      std::unique_ptr<hci> hci_;
  };
}
