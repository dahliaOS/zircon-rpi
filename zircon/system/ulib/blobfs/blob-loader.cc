// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "blob-loader.h"

#include <lib/fzl/owned-vmo-mapper.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <blobfs/common.h>
#include <blobfs/format.h>
#include <fbl/auto_call.h>
#include <fbl/string_buffer.h>

#include "blob-verifier.h"
#include "compression/lz4.h"
#include "compression/zstd.h"
#include "iterator/block-iterator.h"

namespace blobfs {
namespace {

bool PagerCompatible(const Inode& inode) {
  return (inode.header.flags & (kBlobFlagLZ4Compressed | kBlobFlagZSTDCompressed)) == 0;
}

bool IsCompressed(const Inode& inode) {
  return (inode.header.flags & (kBlobFlagLZ4Compressed | kBlobFlagZSTDCompressed)) != 0;
}

constexpr char kBlobVmoNamePrefix[] = "blob";
constexpr char kCompressedTransferBufferVmoNamePrefix[] = "blobCompressed";
constexpr char kBlobMerkleVmoNamePrefix[] = "blob-merkle";

void FormatVmoName(const char* prefix, const Inode& inode,
                   fbl::StringBuffer<ZX_MAX_NAME_LEN>* vmo_name) {
  digest::Digest digest(inode.merkle_root_hash);
  vmo_name->Clear();
  vmo_name->AppendPrintf("%s-%6s", prefix, digest.ToString().c_str());
}

}  // namespace

BlobLoader::BlobLoader(Blobfs* const blobfs) : blobfs_(blobfs) {}

zx_status_t BlobLoader::LoadBlob(uint32_t node_index, fzl::OwnedVmoMapper* data_out,
                                 fzl::OwnedVmoMapper* merkle_out) {
  TRACE_DURATION("blobfs", "BlobLoader::LoadBlob");

  const Inode* const inode = blobfs_->GetNode(node_index);
  if (!inode->header.IsInode() || !inode->header.IsAllocated()) {
    FS_TRACE_ERROR("Attempted to load invalid node %u\n", node_index);
    return ZX_ERR_INVALID_ARGS;
  }

  uint64_t data_blocks = BlobDataBlocks(*inode);
  uint64_t merkle_blocks = MerkleTreeBlocks(*inode);

  if (data_blocks == 0) {
    // Nothing to load.
    return ZX_OK;
  }
  if (merkle_blocks == 0) {
    FS_TRACE_ERROR("Attempted to load blob with no merkle tree");
    return ZX_ERR_IO_DATA_INTEGRITY;
  }

  size_t data_vmo_size, merkle_vmo_size;
  if (mul_overflow(data_blocks, kBlobfsBlockSize, &data_vmo_size)) {
    FS_TRACE_ERROR("Multiplication overflow");
    return ZX_ERR_OUT_OF_RANGE;
  }
  if (mul_overflow(merkle_blocks, kBlobfsBlockSize, &merkle_vmo_size)) {
    FS_TRACE_ERROR("Multiplication overflow");
    return ZX_ERR_OUT_OF_RANGE;
  }

  fbl::StringBuffer<ZX_MAX_NAME_LEN> data_vmo_name, merkle_vmo_name;
  FormatVmoName(kBlobVmoNamePrefix, *inode, &data_vmo_name);
  FormatVmoName(kBlobMerkleVmoNamePrefix, *inode, &merkle_vmo_name);

  fzl::OwnedVmoMapper data_mapper, merkle_mapper;
  zx_status_t status = data_mapper.CreateAndMap(data_vmo_size, data_vmo_name.c_str());
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Failed to initialize vmo; error: %s\n", zx_status_get_string(status));
    return status;
  }
  status = merkle_mapper.CreateAndMap(merkle_vmo_size, merkle_vmo_name.c_str());
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Failed to initialize vmo; error: %s\n", zx_status_get_string(status));
    return status;
  }

  if ((status = LoadMerkle(node_index, *inode, merkle_mapper)) != ZX_OK) {
    return status;
  }
  status = IsCompressed(*inode)
      ? LoadAndDecompressData(node_index, *inode, data_mapper)
      : LoadData(node_index, *inode, data_mapper);
  if (status != ZX_OK) {
    return status;
  }

  BlobVerifier verifier;
  if ((status =
       BlobVerifier::Create(digest::Digest(inode->merkle_root_hash), merkle_mapper.start(),
                            merkle_mapper.size(), data_mapper.size(), &verifier)) != ZX_OK) {
    return status;
  }
  if ((status = verifier.Verify(data_mapper.start(), inode->blob_size)) != ZX_OK) {
    return status;
  }

  *data_out = std::move(data_mapper);
  *merkle_out = std::move(merkle_mapper);
  return ZX_OK;
}

zx_status_t BlobLoader::LoadBlobPaged(uint32_t node_index,
                                      std::unique_ptr<PageWatcher>* page_watcher_out,
                                      fzl::OwnedVmoMapper* data_out,
                                      fzl::OwnedVmoMapper* merkle_out) {
  TRACE_DURATION("blobfs", "BlobLoader::LoadBlobPaged");

  const Inode* const inode = blobfs_->GetNode(node_index);
  if (!inode->header.IsInode() || !inode->header.IsAllocated()) {
    FS_TRACE_ERROR("Attempted to load invalid node %u\n", node_index);
    return ZX_ERR_INVALID_ARGS;
  }

  uint64_t data_blocks = BlobDataBlocks(*inode);
  uint64_t merkle_blocks = MerkleTreeBlocks(*inode);

  if (data_blocks == 0) {
    // Nothing to load.
    return ZX_OK;
  }
  if (merkle_blocks == 0) {
    FS_TRACE_ERROR("Attempted to load blob with no merkle tree");
    return ZX_ERR_IO_DATA_INTEGRITY;
  }

  size_t data_vmo_size, merkle_vmo_size;
  if (mul_overflow(data_blocks, kBlobfsBlockSize, &data_vmo_size)) {
    FS_TRACE_ERROR("Multiplication overflow");
    return ZX_ERR_OUT_OF_RANGE;
  }
  if (mul_overflow(merkle_blocks, kBlobfsBlockSize, &merkle_vmo_size)) {
    FS_TRACE_ERROR("Multiplication overflow");
    return ZX_ERR_OUT_OF_RANGE;
  }

  fbl::StringBuffer<ZX_MAX_NAME_LEN> data_vmo_name, merkle_vmo_name;
  FormatVmoName(kBlobVmoNamePrefix, *inode, &data_vmo_name);
  FormatVmoName(kBlobMerkleVmoNamePrefix, *inode, &merkle_vmo_name);

  fzl::OwnedVmoMapper data_mapper, merkle_mapper;
  auto page_watcher = std::make_unique<PageWatcher>(blobfs_, node_index);
  zx::vmo data_vmo;
  zx_status_t status = page_watcher->CreatePagedVmo(data_vmo_size, &data_vmo);
  if (status != ZX_OK) {
    return status;
  }
  data_vmo.set_property(ZX_PROP_NAME, data_vmo_name.c_str(), data_vmo_name.length());
  status = data_mapper.Map(std::move(data_vmo));

  status = merkle_mapper.CreateAndMap(merkle_vmo_size, merkle_vmo_name.c_str());
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Failed to initialize vmo; error: %s\n", zx_status_get_string(status));
    return status;
  }

  if ((status = LoadMerkle(node_index, *inode, merkle_mapper)) != ZX_OK) {
    return status;
  }

  *page_watcher_out = std::move(page_watcher);
  *data_out = std::move(data_mapper);
  *merkle_out = std::move(merkle_mapper);
  return ZX_OK;
}

zx_status_t BlobLoader::LoadMerkle(uint32_t node_index, const Inode& inode,
                                   const fzl::OwnedVmoMapper& vmo) const {
  TRACE_DURATION("blobfs", "BlobLoader::LoadMerkle");
  vmoid_t vmoid;
  zx_status_t status;
  if ((status = blobfs_->AttachVmo(vmo.vmo(), &vmoid)) != ZX_OK) {
    FS_TRACE_ERROR("Failed to attach VMO to block device; error: %s\n",
                   zx_status_get_string(status));
    return status;
  }
  auto detach = fbl::MakeAutoCall([this, &vmoid]() { blobfs_->DetachVmo(vmoid); });

  fs::ReadTxn txn(blobfs_);

  const uint64_t kMerkleStart = DataStartBlock(blobfs_->Info());
  uint32_t merkle_blocks = MerkleTreeBlocks(inode);
  AllocatedExtentIterator extent_iter(blobfs_->GetNodeFinder(), node_index);
  BlockIterator block_iter(&extent_iter);
  status = StreamBlocks(&block_iter, merkle_blocks,
                        [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
                            txn.Enqueue(vmoid, vmo_offset, dev_offset + kMerkleStart, length);
                            return ZX_OK;
                        });
  if (status != ZX_OK) {
    return status;
  }

  if ((status = txn.Transact()) != ZX_OK) {
    FS_TRACE_ERROR("Failed to flush read transaction: %d\n", status);
    return status;
  }

  return ZX_OK;
}

zx_status_t BlobLoader::LoadData(uint32_t node_index, const Inode& inode,
                                 const fzl::OwnedVmoMapper& vmo) const {
  TRACE_DURATION("blobfs", "BlobLoader::LoadData");
  vmoid_t vmoid;
  zx_status_t status;
  if ((status = blobfs_->AttachVmo(vmo.vmo(), &vmoid)) != ZX_OK) {
    FS_TRACE_ERROR("Failed to attach VMO to block device; error: %s\n",
                   zx_status_get_string(status));
    return status;
  }
  auto detach = fbl::MakeAutoCall([this, &vmoid]() { blobfs_->DetachVmo(vmoid); });

  fs::ReadTxn txn(blobfs_);

  uint32_t merkle_blocks = MerkleTreeBlocks(inode);
  uint32_t data_blocks = inode.block_count - merkle_blocks;
  const uint64_t kDataStart = DataStartBlock(blobfs_->Info()) + merkle_blocks;
  AllocatedExtentIterator extent_iter(blobfs_->GetNodeFinder(), node_index);
  BlockIterator block_iter(&extent_iter);
  status = StreamBlocks(&block_iter, data_blocks,
                        [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
                            txn.Enqueue(vmoid, vmo_offset, dev_offset + kDataStart, length);
                            return ZX_OK;
                        });
  if (status != ZX_OK) {
    return status;
  }

  if ((status = txn.Transact()) != ZX_OK) {
    FS_TRACE_ERROR("Failed to flush read transaction: %d\n", status);
    return status;
  }

  return ZX_OK;
}

zx_status_t BlobLoader::LoadAndDecompressData(uint32_t node_index, const Inode& inode,
                                              const fzl::OwnedVmoMapper& vmo) const {
  TRACE_DURATION("blobfs", "BlobLoader::LoadData");
  uint32_t merkle_blocks = MerkleTreeBlocks(inode);
  uint32_t data_blocks = inode.block_count - merkle_blocks;
  size_t compressed_size;
  if (mul_overflow(data_blocks, kBlobfsBlockSize, &compressed_size)) {
    FS_TRACE_ERROR("Multiplication overflow\n");
    return ZX_ERR_OUT_OF_RANGE;
  }

  fs::ReadTxn txn(blobfs_);

  fbl::StringBuffer<ZX_MAX_NAME_LEN> vmo_name;
  FormatVmoName(kCompressedTransferBufferVmoNamePrefix, inode, &vmo_name);

  fzl::OwnedVmoMapper compressed_mapper;
  zx_status_t status = compressed_mapper.CreateAndMap(compressed_size, vmo_name.c_str());
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Failed to initialized compressed vmo; error: %d\n", status);
    return status;
  }
  vmoid_t compressed_vmoid;
  status = blobfs_->AttachVmo(compressed_mapper.vmo(), &compressed_vmoid);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Failed to attach compressed VMO to blkdev: %d\n", status);
    return status;
  }

  auto detach =
      fbl::MakeAutoCall([this, &compressed_vmoid]() { blobfs_->DetachVmo(compressed_vmoid); });

  const uint64_t kDataStart = DataStartBlock(blobfs_->Info()) + merkle_blocks;
  AllocatedExtentIterator extent_iter(blobfs_->GetNodeFinder(), node_index);
  BlockIterator block_iter(&extent_iter);
  status = StreamBlocks(&block_iter, data_blocks,
                        [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
                            txn.Enqueue(compressed_vmoid, vmo_offset, dev_offset + kDataStart,
                                        length);
                            return ZX_OK;
                        });
  if (status != ZX_OK) {
    return status;
  }

  if ((status = txn.Transact()) != ZX_OK) {
    FS_TRACE_ERROR("Failed to flush read transaction: %d\n", status);
    return status;
  }

  void* target_buffer = vmo.start();
  const void* compressed_buffer = compressed_mapper.start();
  size_t target_size = inode.blob_size;
  if (inode.header.flags & kBlobFlagLZ4Compressed) {
    status = LZ4Decompress(target_buffer, &target_size, compressed_buffer, &compressed_size);
  } else if (inode.header.flags & kBlobFlagZSTDCompressed) {
    status = ZSTDDecompress(target_buffer, &target_size, compressed_buffer, &compressed_size);
  } else {
    FS_TRACE_ERROR("Unsupported compression scheme in flags (%x)\n", inode.header.flags);
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (status != ZX_OK) {
    FS_TRACE_ERROR("Failed to decompress data: %s\n", zx_status_get_string(status));
    return status;
  } else if (target_size != inode.blob_size) {
    FS_TRACE_ERROR("Failed to fully decompress blob (%zu of %zu expected)\n", target_size,
                   inode.blob_size);
    return ZX_ERR_IO_DATA_INTEGRITY;
  }

  return ZX_OK;
}


}  // namespace blobfs
