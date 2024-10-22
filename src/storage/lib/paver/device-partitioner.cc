// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device-partitioner.h"

#include <lib/fdio/fd.h>
#include <lib/zx/status.h>
#include <zircon/errors.h>

#include <memory>
#include <utility>

#include <fbl/span.h>
#include <fbl/string.h>
#include <fbl/string_printf.h>
#include <gpt/gpt.h>

#include "src/lib/uuid/uuid.h"
#include "src/storage/lib/paver/pave-logging.h"
#include "src/storage/lib/paver/utils.h"

namespace paver {

using uuid::Uuid;

const char* PartitionName(Partition type) {
  switch (type) {
    case Partition::kBootloader:
      return GUID_EFI_NAME;
    case Partition::kZirconA:
      return GUID_ZIRCON_A_NAME;
    case Partition::kZirconB:
      return GUID_ZIRCON_B_NAME;
    case Partition::kZirconR:
      return GUID_ZIRCON_R_NAME;
    case Partition::kVbMetaA:
      return GUID_VBMETA_A_NAME;
    case Partition::kVbMetaB:
      return GUID_VBMETA_B_NAME;
    case Partition::kVbMetaR:
      return GUID_VBMETA_R_NAME;
    case Partition::kAbrMeta:
      return GUID_ABR_META_NAME;
    case Partition::kFuchsiaVolumeManager:
      return GUID_FVM_NAME;
    default:
      return "Unknown";
  }
}

zx::status<Uuid> PartitionUuid(Partition type) {
  switch (type) {
    case Partition::kBootloader:
      return zx::ok(Uuid(GUID_BOOTLOADER_VALUE));
    case Partition::kZirconA:
      return zx::ok(Uuid(GUID_ZIRCON_A_VALUE));
    case Partition::kZirconB:
      return zx::ok(Uuid(GUID_ZIRCON_B_VALUE));
    case Partition::kZirconR:
      return zx::ok(Uuid(GUID_ZIRCON_R_VALUE));
    case Partition::kVbMetaA:
      return zx::ok(Uuid(GUID_VBMETA_A_VALUE));
    case Partition::kVbMetaB:
      return zx::ok(Uuid(GUID_VBMETA_B_VALUE));
    case Partition::kVbMetaR:
      return zx::ok(Uuid(GUID_VBMETA_R_VALUE));
    case Partition::kAbrMeta:
      return zx::ok(Uuid(GUID_ABR_META_VALUE));
    case Partition::kFuchsiaVolumeManager:
      return zx::ok(Uuid(GUID_FVM_VALUE));
    default:
      return zx::error(ZX_ERR_NOT_SUPPORTED);
  }
}

fbl::String PartitionSpec::ToString() const {
  if (content_type.empty()) {
    return PartitionName(partition);
  }
  return fbl::StringPrintf("%s (%.*s)", PartitionName(partition),
                           static_cast<int>(content_type.size()), content_type.data());
}

std::vector<std::unique_ptr<DevicePartitionerFactory>>*
DevicePartitionerFactory::registered_factory_list() {
  static std::vector<std::unique_ptr<DevicePartitionerFactory>>* registered_factory_list = nullptr;
  if (registered_factory_list == nullptr) {
    registered_factory_list = new std::vector<std::unique_ptr<DevicePartitionerFactory>>();
  }
  return registered_factory_list;
}

std::unique_ptr<DevicePartitioner> DevicePartitionerFactory::Create(
    fbl::unique_fd devfs_root, const zx::channel& svc_root, Arch arch,
    std::shared_ptr<Context> context, zx::channel block_device) {
  fbl::unique_fd block_dev;
  if (block_device) {
    int fd;
    zx_status_t status = fdio_fd_create(block_device.release(), &fd);
    if (status != ZX_OK) {
      ERROR(
          "Unable to create fd from block_device channel. Does it implement fuchsia.io.Node?: %s\n",
          zx_status_get_string(status));
      return nullptr;
    }
    block_dev.reset(fd);
  }

  for (auto& factory : *registered_factory_list()) {
    if (auto status = factory->New(devfs_root.duplicate(), svc_root, arch, context, block_dev);
        status.is_ok()) {
      return std::move(status.value());
    }
  }
  return nullptr;
}

void DevicePartitionerFactory::Register(std::unique_ptr<DevicePartitionerFactory> factory) {
  registered_factory_list()->push_back(std::move(factory));
}

/*====================================================*
 *               FIXED PARTITION MAP                  *
 *====================================================*/

zx::status<std::unique_ptr<DevicePartitioner>> FixedDevicePartitioner::Initialize(
    fbl::unique_fd devfs_root) {
  if (HasSkipBlockDevice(devfs_root)) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }
  LOG("Successfully initialized FixedDevicePartitioner Device Partitioner\n");
  return zx::ok(new FixedDevicePartitioner(std::move(devfs_root)));
}

bool FixedDevicePartitioner::SupportsPartition(const PartitionSpec& spec) const {
  const PartitionSpec supported_specs[] = {PartitionSpec(paver::Partition::kBootloader),
                                           PartitionSpec(paver::Partition::kZirconA),
                                           PartitionSpec(paver::Partition::kZirconB),
                                           PartitionSpec(paver::Partition::kZirconR),
                                           PartitionSpec(paver::Partition::kVbMetaA),
                                           PartitionSpec(paver::Partition::kVbMetaB),
                                           PartitionSpec(paver::Partition::kVbMetaR),
                                           PartitionSpec(paver::Partition::kAbrMeta),
                                           PartitionSpec(paver::Partition::kFuchsiaVolumeManager)};

  for (const auto& supported : supported_specs) {
    if (SpecMatches(spec, supported)) {
      return true;
    }
  }

  return false;
}

zx::status<std::unique_ptr<PartitionClient>> FixedDevicePartitioner::AddPartition(
    const PartitionSpec& spec) const {
  ERROR("Cannot add partitions to a fixed-map partition device\n");
  return zx::error(ZX_ERR_NOT_SUPPORTED);
}

zx::status<std::unique_ptr<PartitionClient>> FixedDevicePartitioner::FindPartition(
    const PartitionSpec& spec) const {
  if (!SupportsPartition(spec)) {
    ERROR("Unsupported partition %s\n", spec.ToString().c_str());
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  zx::status<Uuid> type_or = PartitionUuid(spec.partition);
  if (type_or.is_error()) {
    ERROR("partition_type is invalid!\n");
    return type_or.take_error();
  }
  Uuid type = type_or.value();

  zx::status<zx::channel> partition =
      OpenBlockPartition(devfs_root_, std::nullopt, type, ZX_SEC(5));
  if (partition.is_error()) {
    return partition.take_error();
  }

  return zx::ok(new BlockPartitionClient(std::move(partition.value())));
}

zx::status<> FixedDevicePartitioner::WipeFvm() const {
  if (auto status = WipeBlockPartition(devfs_root_, std::nullopt, Uuid(GUID_FVM_VALUE));
      status.is_error()) {
    ERROR("Failed to wipe FVM.\n");
  } else {
    LOG("Wiped FVM successfully.\n");
  }
  LOG("Immediate reboot strongly recommended\n");
  return zx::ok();
}

zx::status<> FixedDevicePartitioner::InitPartitionTables() const {
  return zx::error(ZX_ERR_NOT_SUPPORTED);
}

zx::status<> FixedDevicePartitioner::WipePartitionTables() const {
  return zx::error(ZX_ERR_NOT_SUPPORTED);
}

zx::status<> FixedDevicePartitioner::ValidatePayload(const PartitionSpec& spec,
                                                     fbl::Span<const uint8_t> data) const {
  if (!SupportsPartition(spec)) {
    ERROR("Unsupported partition %s\n", spec.ToString().c_str());
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  return zx::ok();
}

zx::status<std::unique_ptr<DevicePartitioner>> DefaultPartitionerFactory::New(
    fbl::unique_fd devfs_root, const zx::channel& svc_root, Arch arch,
    std::shared_ptr<Context> context, const fbl::unique_fd& block_device) {
  return FixedDevicePartitioner::Initialize(std::move(devfs_root));
}

}  // namespace paver
