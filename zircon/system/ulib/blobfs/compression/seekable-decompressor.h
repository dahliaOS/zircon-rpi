// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_BLOBFS_COMPRESSION_SEEKABLE_DECOMPRESSOR_H_
#define ZIRCON_SYSTEM_ULIB_BLOBFS_COMPRESSION_SEEKABLE_DECOMPRESSOR_H_

#include <zircon/types.h>

#include <memory>

#include <stddef.h>

#include <fbl/macros.h>

#include "algorithm.h"

namespace blobfs {

struct CompressionMapping {
  size_t compressed_offset;
  size_t compressed_length;
  size_t decompressed_offset;
  size_t decompressed_length;
};

// A `SeekableDecompressor` is used to decompress parts of blobs transparently. See `Compressor`
// documentation for properties of `Compressor`/`SeekableDecompressor` pair implementations.
class SeekableDecompressor {
 public:
  SeekableDecompressor() = default;
  virtual ~SeekableDecompressor() = default;
  DISALLOW_COPY_ASSIGN_AND_MOVE(SeekableDecompressor);

  virtual zx_status_t DecompressRange(void* uncompressed_buf, size_t* uncompressed_size,
                                      const void* compressed_buf, size_t compressed_buff_size,
                                      size_t offset) = 0;

  virtual zx_status_t MappingForDecompressedAddress(size_t offset, CompressionMapping* map) = 0;
};

}  // namespace blobfs

#endif  // ZIRCON_SYSTEM_ULIB_BLOBFS_COMPRESSION_SEEKABLE_DECOMPRESSOR_H_
