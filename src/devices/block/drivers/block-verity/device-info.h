// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_BLOCK_DRIVERS_BLOCK_VERITY_DEVICE_INFO_H_
#define SRC_STORAGE_BLOCK_DRIVERS_BLOCK_VERITY_DEVICE_INFO_H_

#include <lib/zx/vmo.h>
#include <stddef.h>
#include <stdint.h>
#include <zircon/types.h>

#include <ddk/device.h>
#include <ddktl/protocol/block.h>
//#include <ddktl/protocol/block/partition.h>
//#include <ddktl/protocol/block/volume.h>
#include <fbl/macros.h>

namespace block_verity {

// |block_verity::DeviceInfo| bundles block device configuration details passed from the controller to
// the device.  It is used a const struct in |block_verity::Device| to allow rapid, lock-free access.
struct DeviceInfo {
  // Callbacks to the parent's block protocol methods.
  ddk::BlockProtocolClient block_protocol;
  //ddk::BlockPartitionProtocolClient partition_protocol;
  //ddk::BlockVolumeProtocolClient volume_protocol;
  // The parent block device
  zx_device_t* block_device;

  // The parent device's block information
  uint32_t block_size;

  // The parent device's block count
  uint64_t block_count;

  // This device's required block_op_t size.
  size_t op_size;

  // The number of blocks reserved for each purpose.
  uint64_t superblocks;
  uint64_t integrity_blocks;
  uint64_t data_blocks;

  // A memory region used for processing I/O transactions.
  //zx::vmo vmo;
  // Base address of the VMAR backing the VMO.
  //uint8_t* base;

  DeviceInfo(zx_device_t* device);
  DeviceInfo(DeviceInfo&& other);
  ~DeviceInfo();
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(DeviceInfo);

  // Returns true if the block device can be used by block_verity.  This may fail, for example, if
  // the constructor was unable to get a valid block protocol.
  bool IsValid() const;

  // TODO: remove
  // Reserves a memory region to be used for encrypting and decrypting I/O transactions.  The
  // region will be backed by |vmo| and mapped to |base|.  It will be automatically unmapped when
  // upon this object's destruction.
  //zx_status_t Reserve(size_t size);
};

}  // namespace block_verity

#endif  // SRC_STORAGE_BLOCK_DRIVERS_BLOCK_VERITY_DEVICE_INFO_H_

