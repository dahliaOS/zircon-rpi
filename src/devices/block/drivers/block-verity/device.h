// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_BLOCK_DRIVERS_BLOCK_VERITY_DEVICE_H_
#define SRC_STORAGE_BLOCK_DRIVERS_BLOCK_VERITY_DEVICE_H_

#include <ddk/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/block.h>
#include <fbl/mutex.h>
#include <zircon/device/block.h>

#include "device-info.h"

namespace block_verity {

// See ddk::Device in ddktl/device.h
class Device;
using DeviceType = ddk::Device<
    Device,
    ddk::GetProtocolable,
    ddk::GetSizable,
    ddk::UnbindableDeprecated>;

class Device : public DeviceType,
               public ddk::BlockImplProtocol<Device, ddk::base_protocol> {

 public:
  Device(zx_device_t* parent, DeviceInfo info);
  virtual ~Device();

  // The body of the |Init| thread.  This method uses the unsealed |volume| to
  // start cryptographic workers for normal operation.
  //zx_status_t Init(const DdkVolume& volume) __TA_EXCLUDES(mtx_);

  // ddk::Device methods; see ddktl/device.h
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out);
  zx_off_t DdkGetSize();
  void DdkUnbindDeprecated();
  void DdkRelease();

  // ddk::BlockProtocol methods; see ddktl/protocol/block.h
  void BlockImplQuery(block_info_t* out_info, size_t* out_op_size);
  void BlockImplQueue(block_op_t* block_op, block_impl_queue_callback completion_cb, void* cookie)
      __TA_EXCLUDES(mtx_);

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(Device);

  fbl::Mutex mtx_;
  ddk::BlockProtocolClient block_client_;

};

}  // namespace block_verity

#endif  // SRC_STORAGE_BLOCK_DRIVERS_BLOCK_VERITY_DEVICE_H_
