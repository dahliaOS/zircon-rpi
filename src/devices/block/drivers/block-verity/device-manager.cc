// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device-manager.h"

#include <fuchsia/hardware/block/verified/llcpp/fidl.h>
#include <threads.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <memory>

//#include <crypto/secret.h>
#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/macros.h>
//#include <zxcrypt/ddk-volume.h>
//#include <zxcrypt/volume.h>

//#include "device-info.h"
#include "device.h"

namespace block_verity {

zx_status_t DeviceManager::Create(void* ctx, zx_device_t* parent) {
  zx_status_t rc;
  fbl::AllocChecker ac;

  auto manager = fbl::make_unique_checked<DeviceManager>(&ac, parent);
  if (!ac.check()) {
    zxlogf(ERROR, "failed to allocate %zu bytes", sizeof(DeviceManager));
    return ZX_ERR_NO_MEMORY;
  }

  if ((rc = manager->Bind()) != ZX_OK) {
    zxlogf(ERROR, "failed to bind: %s", zx_status_get_string(rc));
    return rc;
  }

  // devmgr is now in charge of the memory for |manager|.
  __UNUSED auto* owned_by_devmgr_now = manager.release();

  return ZX_OK;
}

zx_status_t DeviceManager::Bind() {
  zx_status_t rc;
  fbl::AutoLock lock(&mtx_);

  if ((rc = DdkAdd("verity")) != ZX_OK) {
    zxlogf(ERROR, "failed to add device: %s", zx_status_get_string(rc));
    state_ = kRemoved;
    return rc;
  }

  state_ = kSealed;
  return ZX_OK;
}

void DeviceManager::DdkUnbindDeprecated() {
  fbl::AutoLock lock(&mtx_);
  if (state_ == kBinding) {
    state_ = kUnbinding;
  } else if (state_ == kSealed || state_ == kUnsealed || state_ == kShredded) {
    state_ = kRemoved;
    DdkRemoveDeprecated();
  }
}

void DeviceManager::DdkRelease() { delete this; }

zx_status_t DeviceManager::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  ::fidl::Transaction* transaction = reinterpret_cast<::fidl::Transaction*>(txn);
  bool success = llcpp::fuchsia::hardware::block::verified::DeviceManager::Dispatch(
      this, msg, transaction);
  return success ? ZX_OK : ZX_ERR_NOT_SUPPORTED;
}

void DeviceManager::OpenForWrite(::llcpp::fuchsia::hardware::block::verified::Config config,
                                 OpenForWriteCompleter::Sync completer) {
  fbl::AutoLock lock(&mtx_);

  block_info_t blk;
  size_t op_size;
  ddk::BlockProtocolClient block_protocol_client(parent());
  block_protocol_client.Query(&blk, &op_size);

  // TODO:
  // Check that the config is entirely valid
  // and that the config matches the underlying block size

  // Create the mutable device
  fbl::AllocChecker ac;
  DeviceInfo info(parent());
  auto device = fbl::make_unique_checked<block_verity::Device>(&ac, zxdev(), std::move(info));
  if (!ac.check()) {
    zxlogf(ERROR, "failed to allocate %zu bytes",
           sizeof(Device));
    completer.ReplyError(ZX_ERR_NO_MEMORY);
    return;
  }

  // TODO: bind

  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void DeviceManager::CloseAndGenerateSeal(CloseAndGenerateSealCompleter::Sync completer) {
  fbl::AutoLock lock(&mtx_);
  ::llcpp::fuchsia::hardware::block::verified::Seal seal;
  // TODO: implement
  //
  // Unbind the mutable child device
  // wait for that child to be removed
  // recompute and write out all verified block data
  // flush
  // return root hash

  // completer.Reply(std::move(seal));
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void DeviceManager::OpenForVerifiedRead(
    ::llcpp::fuchsia::hardware::block::verified::Config config,
    ::llcpp::fuchsia::hardware::block::verified::Seal seal,
    OpenForVerifiedReadCompleter::Sync completer) {
  fbl::AutoLock lock(&mtx_);
  // TODO: create the verified device
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void DeviceManager::Close(CloseCompleter::Sync completer) {
  fbl::AutoLock lock(&mtx_);
  // TODO: implement
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}
/*
zx_status_t DeviceManager::Unseal(const uint8_t* ikm, size_t ikm_len, key_slot_t slot) {
  fbl::AutoLock lock(&mtx_);
  if (state_ != kSealed) {
    zxlogf(ERROR, "can't unseal zxcrypt, state=%d", state_);
    return ZX_ERR_BAD_STATE;
  }
  return UnsealLocked(ikm, ikm_len, slot);
}

zx_status_t DeviceManager::Seal() {
  zx_status_t rc;
  fbl::AutoLock lock(&mtx_);

  if (state_ != kUnsealed && state_ != kShredded) {
    zxlogf(ERROR, "can't seal zxcrypt, state=%d", state_);
    return ZX_ERR_BAD_STATE;
  }
  if ((rc = device_rebind(zxdev())) != ZX_OK) {
    zxlogf(ERROR, "failed to rebind zxcrypt: %s", zx_status_get_string(rc));
    return rc;
  }

  state_ = kSealed;
  return ZX_OK;
}

zx_status_t DeviceManager::Shred() {
  fbl::AutoLock lock(&mtx_);

  // We want to shred the underlying volume, but if we have an unsealed device,
  // we don't mind letting it keep working for now.  Other parts of the system
  // would rather we shut down gracefully than immediately stop permitting reads
  // or acking writes.  So we instantiate a new DdkVolume here, quietly shred
  // it, and let child devices carry on as if nothing happened.
  std::unique_ptr<DdkVolume> volume_to_shred;
  zx_status_t rc;
  rc = DdkVolume::OpenOpaque(parent(), &volume_to_shred);
  if (rc != ZX_OK) {
    zxlogf(ERROR, "failed to open volume to shred: %s", zx_status_get_string(rc));
    return rc;
  }

  rc = volume_to_shred->Shred();
  if (rc != ZX_OK) {
    zxlogf(ERROR, "failed to shred volume: %s", zx_status_get_string(rc));
    return rc;
  }

  state_ = kShredded;
  return ZX_OK;
}

zx_status_t DeviceManager::UnsealLocked(const uint8_t* ikm, size_t ikm_len, key_slot_t slot) {
  zx_status_t rc;

  // Unseal the zxcrypt volume.
  crypto::Secret key;
  uint8_t* buf;
  if ((rc = key.Allocate(ikm_len, &buf)) != ZX_OK) {
    zxlogf(ERROR, "failed to allocate %zu-byte key: %s", ikm_len, zx_status_get_string(rc));
    return rc;
  }
  memcpy(buf, ikm, key.len());
  std::unique_ptr<DdkVolume> volume;
  if ((rc = DdkVolume::Unlock(parent(), key, slot, &volume)) != ZX_OK) {
    zxlogf(ERROR, "failed to unseal volume: %s", zx_status_get_string(rc));
    return rc;
  }

  // Get the parent device's configuration details.
  DeviceInfo info(parent(), *volume);
  if (!info.IsValid()) {
    zxlogf(ERROR, "failed to get valid device info");
    return ZX_ERR_BAD_STATE;
  }
  // Reserve space for shadow I/O transactions
  if ((rc = info.Reserve(Volume::kBufferSize)) != ZX_OK) {
    zxlogf(ERROR, "failed to reserve buffer for I/O: %s", zx_status_get_string(rc));
    return rc;
  }

  // Create the unsealed device
  fbl::AllocChecker ac;
  auto device = fbl::make_unique_checked<zxcrypt::Device>(&ac, zxdev(), std::move(info));
  if (!ac.check()) {
    zxlogf(ERROR, "failed to allocate %zu bytes", sizeof(zxcrypt::Device));
    return ZX_ERR_NO_MEMORY;
  }
  if ((rc = device->Init(*volume)) != ZX_OK) {
    zxlogf(ERROR, "failed to initialize device: %s", zx_status_get_string(rc));
    return rc;
  }
  if ((rc = device->DdkAdd("unsealed")) != ZX_OK) {
    zxlogf(ERROR, "failed to add device: %s", zx_status_get_string(rc));
    return rc;
  }

  // devmgr is now in charge of the memory for |device|
  __UNUSED auto owned_by_devmgr_now = device.release();
  state_ = kUnsealed;
  return ZX_OK;
}
*/

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = DeviceManager::Create;
  return ops;
}();

}  // namespace block_verity

// clang-format off
ZIRCON_DRIVER_BEGIN(block_verity, block_verity::driver_ops, "zircon", "0.1", 2)
    BI_ABORT_IF_AUTOBIND,
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_BLOCK),
ZIRCON_DRIVER_END(block_verity)
// clang-format on
