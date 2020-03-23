// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_BLOBFS_COMPRESSION_CHUNKED_H_
#define ZIRCON_SYSTEM_ULIB_BLOBFS_COMPRESSION_CHUNKED_H_

#include <zircon/types.h>

#include <memory>

#include <blobfs/format.h>
#include <chunked-compression/chunked-decompressor.h>
#include <chunked-compression/streaming-chunked-compressor.h>

#include "compressor.h"
#include "decompressor.h"
#include "seekable-decompressor.h"

namespace blobfs {

// Compressor implementation for the zstd seekable format library implemented in
// //third_party/zstd/contrib/seekable_format. The library provides a convenient API for
// random access in zstd archives.
class ChunkedCompressor : public Compressor {
 public:
  ChunkedCompressor() = delete;
  ~ChunkedCompressor() = default;
  DISALLOW_COPY_ASSIGN_AND_MOVE(ChunkedCompressor);

  static uint32_t InodeHeaderCompressionFlags() { return kBlobFlagChunkCompressed; }

  static zx_status_t Create(size_t input_size, size_t* output_limit_out,
                            std::unique_ptr<ChunkedCompressor>* out);

  // Registers |dst| as the output for compression.
  // Must be called before |Update()| or |Final()| are called.
  zx_status_t SetOutput(void* dst, size_t dst_len);

  ////////////////////////////////////////
  // Compressor interface
  size_t Size() const final;
  zx_status_t Update(const void* input_data, size_t input_length) final;
  zx_status_t End() final;

 private:
  explicit ChunkedCompressor(chunked_compression::StreamingChunkedCompressor compressor,
                             size_t input_len);

  chunked_compression::StreamingChunkedCompressor compressor_;
  size_t input_len_;
  // Set when End() is called to the final output size.
  std::optional<size_t> compressed_size_;
};

class ChunkedDecompressor : public Decompressor, public SeekableDecompressor {
 public:
  ChunkedDecompressor() = default;
  DISALLOW_COPY_ASSIGN_AND_MOVE(ChunkedDecompressor);

  // Decompressor implementation.
  zx_status_t Decompress(void* uncompressed_buf, size_t* uncompressed_size,
                         const void* compressed_buf, const size_t max_compressed_size) final;

  // SeekableDecompressor implementation.
  zx_status_t DecompressRange(void* uncompressed_buf, size_t* uncompressed_size,
                              const void* compressed_buf, size_t max_compressed_size,
                              size_t offset) final;

 private:
  chunked_compression::ChunkedArchiveHeader header_;
  chunked_compression::ChunkedDecompressor decompressor_;
};

}  // namespace blobfs

#endif  // ZIRCON_SYSTEM_ULIB_BLOBFS_COMPRESSION_CHUNKED_H_
