// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chunked.h"

#include <zircon/errors.h>
#include <zircon/status.h>
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
  *out =
      std::unique_ptr<ChunkedCompressor>(new ChunkedCompressor(std::move(compressor), input_size));

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

zx_status_t ChunkedDecompressor::CreateDecompressor(const void* header_buf, size_t header_buf_sz,
                                                    std::unique_ptr<SeekableDecompressor>* out) {
  auto decompressor = std::make_unique<ChunkedDecompressor>();
  if (chunked_compression::ChunkedArchiveHeader::Parse(
          header_buf, header_buf_sz, &decompressor->header_) != chunked_compression::kStatusOk) {
    return ZX_ERR_INTERNAL;
  }
  *out = std::move(decompressor);
  return ZX_OK;
}

zx_status_t ChunkedDecompressor::Decompress(void* uncompressed_buf, size_t* uncompressed_size,
                                            const void* compressed_buf,
                                            const size_t max_compressed_size) {
  TRACE_DURATION("blobfs", "ChunkedCompressor::Duration", "compressed_size", max_compressed_size);
  chunked_compression::ChunkedArchiveHeader header;
  if (chunked_compression::ChunkedArchiveHeader::Parse(compressed_buf, max_compressed_size,
                                                       &header) != chunked_compression::kStatusOk) {
    FS_TRACE_ERROR("blobfs: Invalid archive header.\n");
    return ZX_ERR_IO_DATA_INTEGRITY;
  }
  size_t decompression_buf_size = *uncompressed_size;
  if (decompressor_.Decompress(header, compressed_buf, max_compressed_size, uncompressed_buf,
                               decompression_buf_size,
                               uncompressed_size) != chunked_compression::kStatusOk) {
    FS_TRACE_ERROR("blobfs: Failed to decompress archive.\n");
    return ZX_ERR_IO_DATA_INTEGRITY;
  }
  return ZX_OK;
}

zx_status_t ChunkedDecompressor::DecompressRange(void* uncompressed_buf, size_t* uncompressed_size,
                                                 const void* compressed_buf,
                                                 size_t max_compressed_size,
                                                 size_t offset) {
  TRACE_DURATION("blobfs", "ChunkedCompressor::DecompressRange", "length", *uncompressed_size);
  std::optional<unsigned> first_byte_entry = header_.EntryForDecompressedOffset(offset);
  std::optional<unsigned> last_byte_entry =
      header_.EntryForDecompressedOffset(offset + (*uncompressed_size) - 1);
  if (!first_byte_entry || !last_byte_entry) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  size_t src_offset = 0;
  size_t dst_offset = 0;
  for (unsigned i = *first_byte_entry; i <= *last_byte_entry; ++i) {
    const chunked_compression::SeekTableEntry& entry = header_.SeekTable()[i];

    ZX_DEBUG_ASSERT(src_offset + entry.compressed_size <= max_compressed_size);
    ZX_DEBUG_ASSERT(dst_offset + entry.decompressed_size <= *uncompressed_size);

    const uint8_t* src = static_cast<const uint8_t*>(compressed_buf) + src_offset;
    uint8_t* dst = static_cast<uint8_t*>(uncompressed_buf) + dst_offset;
    size_t bytes_in_frame;
    chunked_compression::Status status =
        decompressor_.DecompressFrame(header_, i, src, max_compressed_size - src_offset, dst,
                                      *uncompressed_size - dst_offset, &bytes_in_frame);
    if (status != chunked_compression::kStatusOk) {
      FS_TRACE_ERROR("blobfs DecompressFrame failed: %s\n",
                     zx_status_get_string(chunked_compression::ToZxStatus(status)));
      return chunked_compression::ToZxStatus(status);
    }
    src_offset += entry.compressed_size;
    dst_offset += bytes_in_frame;
  }
  ZX_ASSERT(dst_offset == *uncompressed_size);
  return ZX_OK;
}

zx_status_t ChunkedDecompressor::MappingForDecompressedAddress(size_t offset,
                                                               CompressionMapping* mapping) {
  std::optional<unsigned> idx = header_.EntryForDecompressedOffset(offset);
  if (!idx) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  mapping->compressed_offset = header_.SeekTable()[*idx].compressed_offset;
  mapping->decompressed_offset = header_.SeekTable()[*idx].decompressed_offset;
  mapping->compressed_length = header_.SeekTable()[*idx].compressed_size;
  mapping->decompressed_length = header_.SeekTable()[*idx].decompressed_size;

  return ZX_OK;
}

}  // namespace blobfs
