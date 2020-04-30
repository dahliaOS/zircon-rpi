// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <string.h>

#include "device-info.h"

namespace block_verity {

// Shared implementation of various routines, with additional hooks for
// block reads and writes, which may be rejected (for readonly devices)
// or may do additional verity checking (for devices opened for verified read)

Device::Device(zx_device_t* parent, DeviceInfo info) : DeviceType(parent)
{
}

Device::~Device() {}


zx_status_t Device::DdkGetProtocol(uint32_t proto_id, void* out) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_off_t Device::DdkGetSize() {
  //TODO: implement
  return 0;
}

void Device::DdkUnbindDeprecated() {
  //TODO: implement
}

void Device::DdkRelease() {
  //TODO: implement
}

void Device::BlockImplQuery(block_info_t* out_info, size_t* out_op_size) {
  // We set the readonly flag for now, I guess?
  memset(out_info, 0, sizeof(block_info_t));

  //out_op_size = 
  //TODO: implement
}

void Device::BlockImplQueue(block_op_t* block_op, block_impl_queue_callback completion_cb, void* cookie) {
  //TODO: implement
}

}  // namespace block_verity
