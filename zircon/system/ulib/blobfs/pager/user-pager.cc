// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "user-pager.h"

#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/fzl/vmo-mapper.h>
#include <limits.h>
#include <zircon/status.h>

#include <memory>

#include <blobfs/format.h>
#include <fbl/auto_call.h>
#include <fs/trace.h>

namespace blobfs {

zx_status_t UserPager::InitPager() {
  TRACE_DURATION("blobfs", "UserPager::InitPager");

  // Make sure blocks are page-aligned.
  static_assert(kBlobfsBlockSize % PAGE_SIZE == 0);
  // Make sure the pager transfer buffer is block-aligned.
  static_assert(kTransferBufferSize % kBlobfsBlockSize == 0);

  // Set up the pager transfer buffer.
  zx_status_t status = zx::vmo::create(kTransferBufferSize, 0, &transfer_buffer_);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Cannot create transfer buffer: %s\n", zx_status_get_string(status));
    return status;
  }
  status = AttachTransferVmo(transfer_buffer_);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to attach transfer vmo: %s\n", zx_status_get_string(status));
    return status;
  }

  // Set up the decompress buffer.
  // XXX share this with page-watcher.cc (or figure out how to match it to the archive frame sz)
  constexpr uint64_t kPrefetchClusterSize = (1024 * (1 << 10));
  status = zx::vmo::create(kPrefetchClusterSize, 0, &decompression_buffer_);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Cannot create decompress buffer: %s\n", zx_status_get_string(status));
    return status;
  }

  // Create the pager.
  status = zx::pager::create(0, &pager_);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Cannot initialize pager\n");
    return status;
  }

  // Start the pager thread.
  status = pager_loop_.StartThread("blobfs-pager-thread");
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Could not start pager thread\n");
    return status;
  }

  return ZX_OK;
}

zx_status_t UserPager::TransferPagesToVmo(uint64_t offset, uint64_t length, const zx::vmo& vmo,
                                          UserPagerInfo* info) {
  TRACE_DURATION("blobfs", "UserPager::TransferPagesToVmo", "offset", offset, "length", length);

  ZX_DEBUG_ASSERT(info);
  // Align the range to include pages needed for verification.
  zx_status_t status = AlignForVerification(&offset, &length, info);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to align requested pages: %s\n", zx_status_get_string(status));
    return status;
  }

  auto decommit = fbl::MakeAutoCall([this, length]() {
    // Decommit pages in the transfer buffer that might have been populated. All blobs share the
    // same transfer buffer - this prevents data leaks between different blobs.
    transfer_buffer_.op_range(ZX_VMO_OP_DECOMMIT, 0, fbl::round_up(length, kBlobfsBlockSize),
                              nullptr, 0);
  });

  if (info->decompressor) {
    CompressionMapping mapping;
    status = info->decompressor->MappingForDecompressedAddress(offset, &mapping);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to find range: %s\n", zx_status_get_string(status));
      return status;
    }
    ZX_DEBUG_ASSERT(mapping.decompressed_length >= kBlobfsBlockSize);
    ZX_DEBUG_ASSERT(mapping.decompressed_length % kBlobfsBlockSize == 0);

    // The compressed frame may not fall at a block aligned address, but we read in block aligned
    // chunks. This offset will be applied to the buffer we pass to decompression.
    size_t offset_of_compressed_data = mapping.compressed_offset % kBlobfsBlockSize;

    // Read from storage into the transfer buffer.
    size_t read_offset = fbl::round_down(mapping.compressed_offset, kBlobfsBlockSize); 
    size_t read_len = (mapping.compressed_length + offset_of_compressed_data);
    status = PopulateTransferVmo(read_offset, read_len, info);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to populate transfer vmo: %s\n", zx_status_get_string(status));
      return status;
    }

    // Map the transfer VMO in order to pass the decompressor a pointer to the data.
    fzl::VmoMapper compressed_mapper;
    zx_status_t status = compressed_mapper.Map(transfer_buffer_, 0, read_len, ZX_VM_PERM_READ);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to map transfer buffer: %s\n", zx_status_get_string(status));
      return status;
    }
    auto unmap_compression = fbl::MakeAutoCall([&]() { compressed_mapper.Unmap(); });

    // Map the decompression VMO.
    fzl::VmoMapper decompressed_mapper;
    if ((status = decompressed_mapper.Map(decompression_buffer_, 0, mapping.decompressed_length,
                                          ZX_VM_PERM_READ | ZX_VM_PERM_WRITE))
        != ZX_OK) {
      FS_TRACE_ERROR("Failed to map decompress buffer: %s\n", zx_status_get_string(status));
      return status;
    }
    auto unmap_decompression = fbl::MakeAutoCall([&]() { decompressed_mapper.Unmap(); });

    size_t decompressed_size = mapping.decompressed_length;
    uint8_t* src = static_cast<uint8_t*>(compressed_mapper.start()) + offset_of_compressed_data;
    status = info->decompressor->DecompressRange(decompressed_mapper.start(), &decompressed_size,
                                                 src, mapping.compressed_length,
                                                 mapping.decompressed_offset);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to decompress: %s\n", zx_status_get_string(status));
      return status;
    }

    // Verify the decompressed pages.
    status = info->verifier->VerifyPartial(decompressed_mapper.start(),
                                           mapping.decompressed_length,
                                           mapping.decompressed_offset);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to verify transfer vmo: %s\n", zx_status_get_string(status));
      return status;
    }

    decompressed_mapper.Unmap();

    ZX_DEBUG_ASSERT(offset % PAGE_SIZE == 0);
    // Move the pages from the transfer buffer to the destination VMO.
    status = pager_.supply_pages(vmo, mapping.decompressed_offset,
                                 fbl::round_up<uint64_t, uint64_t>(mapping.decompressed_length, PAGE_SIZE),
                                 decompression_buffer_, 0);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to supply pages to paged VMO: %s\n",
                     zx_status_get_string(status));
      return status;
    }
  } else {
    // Read from storage into the transfer buffer.
    status = PopulateTransferVmo(offset, length, info);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to populate transfer vmo: %s\n", zx_status_get_string(status));
      return status;
    }

    // Verify the pages read in.
    status = VerifyTransferVmo(offset, length, transfer_buffer_, info);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to verify transfer vmo: %s\n", zx_status_get_string(status));
      return status;
    }

    ZX_DEBUG_ASSERT(offset % PAGE_SIZE == 0);
    // Move the pages from the transfer buffer to the destination VMO.
    status = pager_.supply_pages(vmo, offset, fbl::round_up<uint64_t, uint64_t>(length, PAGE_SIZE),
                                 transfer_buffer_, 0);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: Failed to supply pages to paged VMO: %s\n",
                     zx_status_get_string(status));
      return status;
    }
  }

  return ZX_OK;
}

}  // namespace blobfs
