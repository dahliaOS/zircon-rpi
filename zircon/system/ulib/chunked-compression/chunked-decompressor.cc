// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chunked-compression/chunked-archive.h>
#include <chunked-compression/chunked-decompressor.h>
#include <chunked-compression/status.h>
#include <fbl/array.h>
#include <zstd/zstd.h>

#include "logging.h"

namespace chunked_compression {

struct ChunkedDecompressor::DecompressionContext {
  DecompressionContext() = default;
  explicit DecompressionContext(ZSTD_DCtx* ctx) : inner_(ctx) {}
  ~DecompressionContext() { ZSTD_freeDCtx(inner_); }

  ZSTD_DCtx* inner_;
};

ChunkedDecompressor::ChunkedDecompressor()
    : context_(std::make_unique<DecompressionContext>(ZSTD_createDCtx())) {}
ChunkedDecompressor::~ChunkedDecompressor() {}

Status ChunkedDecompressor::DecompressBytes(const void* data, size_t len,
                                            fbl::Array<uint8_t>* data_out,
                                            size_t* bytes_written_out) {
  Status status;
  ChunkedArchiveHeader header;
  if ((status = ChunkedArchiveHeader::Parse(data, len, &header)) != kStatusOk) {
    FX_LOG(ERROR, kLogTag, "Failed to parse header");
    return status;
  }
  ChunkedDecompressor decompressor;
  size_t out_len = header.DecompressedSize();
  fbl::Array<uint8_t> buf(new uint8_t[out_len], out_len);
  status = decompressor.Decompress(header, data, len, buf.get(), buf.size(), bytes_written_out);
  if (status == kStatusOk) {
    *data_out = std::move(buf);
  }
  return status;
}

Status ChunkedDecompressor::Decompress(const ChunkedArchiveHeader& header, const void* data,
                                       size_t len, void* dst, size_t dst_len,
                                       size_t* bytes_written_out) {
  Status status;
  if (dst_len < header.DecompressedSize()) {
    return kStatusErrBufferTooSmall;
  }

  size_t bytes_written = 0;
  for (unsigned i = 0; i < header.SeekTable().size(); ++i) {
    const SeekTableEntry& entry = header.SeekTable()[i];
    ZX_DEBUG_ASSERT(entry.compressed_offset + entry.compressed_size <= len);
    ZX_DEBUG_ASSERT(entry.decompressed_offset + entry.decompressed_size <= dst_len);
    auto frame_src = (static_cast<const uint8_t*>(data) + entry.compressed_offset);
    auto frame_dst = (static_cast<uint8_t*>(dst) + entry.decompressed_offset);
    size_t frame_decompressed_size;
    if ((status = DecompressFrame(header, i, frame_src, entry.compressed_size, frame_dst,
                                  entry.decompressed_size, &frame_decompressed_size)) !=
        kStatusOk) {
      return status;
    }
    ZX_DEBUG_ASSERT(frame_decompressed_size == entry.decompressed_size);
    bytes_written += frame_decompressed_size;
  }

  ZX_DEBUG_ASSERT(bytes_written == header.DecompressedSize());
  *bytes_written_out = bytes_written;

  return kStatusOk;
}

Status ChunkedDecompressor::DecompressFrame(const ChunkedArchiveHeader& header, unsigned frame_num,
                                            const void* frame_data, size_t frame_len, void* dst,
                                            size_t dst_len, size_t* bytes_written_out) {
  if (frame_num >= header.SeekTable().size()) {
    return kStatusErrInvalidArgs;
  }
  const SeekTableEntry& entry = header.SeekTable()[frame_num];
  if (frame_len < entry.compressed_size || dst_len < entry.decompressed_size) {
    return kStatusErrBufferTooSmall;
  }

  size_t decompressed_size =
      ZSTD_decompressDCtx(context_->inner_, dst, entry.decompressed_size, frame_data,
                          entry.compressed_size);
  if (ZSTD_isError(decompressed_size)) {
    FX_LOGF(ERROR, kLogTag, "Decompression failed: %s", ZSTD_getErrorName(decompressed_size));
    return kStatusErrInternal;
  }

  if (decompressed_size != entry.decompressed_size) {
    FX_LOGF(ERROR, kLogTag, "Decompressed %lu bytes, expected %lu", decompressed_size,
            entry.decompressed_size);
    return kStatusErrIoDataIntegrity;
  }

  *bytes_written_out = decompressed_size;
  return kStatusOk;
}

}  // namespace chunked_compression
