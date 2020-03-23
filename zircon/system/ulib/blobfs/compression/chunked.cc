// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chunked.h"

#include <zircon/errors.h>
#include <zircon/types.h>

#include <chunked-compression/chunked-decompressor.h>
#include <chunked-compression/status.h>
#include <chunked-compression/streaming-chunked-compressor.h>
#include <fs/trace.h>

namespace blobfs {

namespace {

constexpr int kDefaultLevel = 3;

}  // namespace

// ChunkedCompressor

ChunkedCompressor::ChunkedCompressor(chunked_compression::StreamingChunkedCompressor compressor,
                                     size_t input_len)
    : compressor_(std::move(compressor)), input_len_(input_len) {}

zx_status_t ChunkedCompressor::Create(size_t input_size, size_t* output_limit_out,
                                      std::unique_ptr<ChunkedCompressor>* out) {
  chunked_compression::CompressionParams params;
  params.compression_level = kDefaultLevel;
  params.chunk_size = chunked_compression::CompressionParams::ChunkSizeForInputSize(input_size);

  chunked_compression::StreamingChunkedCompressor compressor(params);

  *output_limit_out = compressor.ComputeOutputSizeLimit(input_size);
  *out = std::unique_ptr<ChunkedCompressor>(
      new ChunkedCompressor(std::move(compressor), input_size));

  return ZX_OK;
}

zx_status_t ChunkedCompressor::SetOutput(void* dst, size_t dst_len) {
  if (dst_len < compressor_.ComputeOutputSizeLimit(input_len_)) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  }
  if (compressor_.Init(input_len_, dst, dst_len) != chunked_compression::kStatusOk) {
    FS_TRACE_ERROR("blobfs: Failed to initialize compressor\n");
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

size_t ChunkedCompressor::Size() const { return compressed_size_.value_or(0ul); }

zx_status_t ChunkedCompressor::Update(const void* input_data, size_t input_length) {
  TRACE_DURATION("blobfs", "ChunkedCompressor::Update", "input_length", input_length);
  if (compressor_.Update(input_data, input_length) != chunked_compression::kStatusOk) {
    FS_TRACE_ERROR("blobfs: Compression failed.\n");
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

zx_status_t ChunkedCompressor::End() {
  TRACE_DURATION("blobfs", "ChunkedCompressor::End");
  size_t sz;
  if (compressor_.Final(&sz) != chunked_compression::kStatusOk) {
    FS_TRACE_ERROR("blobfs: Compression failed.\n");
    return ZX_ERR_INTERNAL;
  }
  compressed_size_ = sz;
  return ZX_OK;
}

// ChunkedDecompressor

zx_status_t ChunkedDecompressor::Decompress(
    void* uncompressed_buf, size_t* uncompressed_size, const void* compressed_buf,
    const size_t max_compressed_size) {
  TRACE_DURATION("blobfs", "ChunkedCompressor::Duration", "compressed_size", max_compressed_size);
  chunked_compression::ChunkedArchiveHeader header;
  if (chunked_compression::ChunkedArchiveHeader::Parse(compressed_buf, max_compressed_size, &header)
      != chunked_compression::kStatusOk) {
    FS_TRACE_ERROR("blobfs: Invalid archive header.\n");
    return ZX_ERR_IO_DATA_INTEGRITY;
  }
  size_t decompression_buf_size = *uncompressed_size;
  if (decompressor_.Decompress(header, compressed_buf, max_compressed_size, uncompressed_buf,
                               decompression_buf_size, uncompressed_size)
      != chunked_compression::kStatusOk) {
    FS_TRACE_ERROR("blobfs: Failed to decompress archive.\n");
    return ZX_ERR_IO_DATA_INTEGRITY;
  }
  return ZX_OK;
}

zx_status_t ChunkedDecompressor::DecompressRange(
    void* uncompressed_buf, size_t* uncompressed_size, const void* compressed_buf,
    size_t max_compressed_size, size_t offset) {
  // TODO: Need to ensure the seek table is loaded at this point, or lazily load it on the first
  // invocation.
  // Key challenge: How big is the seek table?
  ZX_ASSERT(false); // Unimplemented!
}

}  // namespace blobfs
