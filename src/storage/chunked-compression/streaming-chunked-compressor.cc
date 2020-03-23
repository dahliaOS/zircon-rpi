// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "streaming-chunked-compressor.h"

#include <zircon/assert.h>

#include <algorithm>

#include <fbl/algorithm.h>
#include <zstd/zstd.h>

#include "src/lib/fxl/logging.h"
#include "src/storage/chunked-compression/chunked-archive.h"
#include "src/storage/chunked-compression/chunked-compressor.h"
#include "src/storage/chunked-compression/status.h"

namespace chunked_compression {

struct StreamingChunkedCompressor::CompressionContext {
  CompressionContext() {}
  explicit CompressionContext(ZSTD_CCtx* ctx) : inner_(ctx) {}
  ~CompressionContext() { ZSTD_freeCCtx(inner_); }

  size_t current_output_frame_start_;
  size_t current_output_frame_relative_pos_;

  ZSTD_CCtx* inner_;
};

StreamingChunkedCompressor::StreamingChunkedCompressor()
    : StreamingChunkedCompressor(CompressionParams{}) {}

StreamingChunkedCompressor::StreamingChunkedCompressor(CompressionParams params)
    : params_(params), context_(std::make_unique<CompressionContext>(ZSTD_createCCtx())) {}

StreamingChunkedCompressor::~StreamingChunkedCompressor() {}

StreamingChunkedCompressor::StreamingChunkedCompressor(StreamingChunkedCompressor&& o)
    : context_(std::move(o.context_)) {}

StreamingChunkedCompressor& StreamingChunkedCompressor::operator=(StreamingChunkedCompressor&& o) {
  context_ = std::move(o.context_);
  return *this;
}

size_t StreamingChunkedCompressor::ComputeOutputSizeLimit(size_t len) {
  if (len == 0) {
    return 0ul;
  }
  const size_t num_frames = ChunkedArchiveWriter::NumFramesForDataSize(len, params_.chunk_size);
  size_t size = ChunkedArchiveWriter::MetadataSizeForNumFrames(num_frames);
  size += (ZSTD_compressBound(params_.chunk_size) * num_frames);
  return size;
}

Status StreamingChunkedCompressor::Init(size_t data_len, void* dst, size_t dst_len) {
  size_t num_frames = ChunkedArchiveWriter::NumFramesForDataSize(data_len, params_.chunk_size);
  size_t metadata_size = ChunkedArchiveWriter::MetadataSizeForNumFrames(num_frames);
  if (metadata_size > dst_len) {
    return kStatusErrBufferTooSmall;
  }

  size_t r = ZSTD_initCStream(context_->inner_, params_.compression_level);
  if (ZSTD_isError(r)) {
    FXL_LOG(ERROR) << "Failed to init stream";
    return kStatusErrInternal;
  }

  compressed_output_ = static_cast<uint8_t*>(dst);
  compressed_output_len_ = dst_len;
  compressed_output_offset_ = metadata_size;

  input_len_ = data_len;
  input_offset_ = 0ul;

  writer_ = std::make_unique<ChunkedArchiveWriter>(dst, dst_len, num_frames);

  return kStatusOk;
}

Status StreamingChunkedCompressor::Update(const void* data, size_t len) {
  if (compressed_output_ == nullptr) {
    return kStatusErrBadState;
  }

  size_t consumed = 0;
  // Consume data up to one input frame at a time.
  while (consumed < len) {
    const size_t bytes_left = len - consumed;

    const size_t current_frame_start = fbl::round_down(input_offset_, params_.chunk_size);
    const size_t current_frame_end = fbl::min(current_frame_start + params_.chunk_size, input_len_);
    const size_t bytes_left_in_current_frame = current_frame_end - input_offset_;

    const size_t bytes_to_consume = fbl::min(bytes_left, bytes_left_in_current_frame);

    Status status = AppendToFrame(static_cast<const uint8_t*>(data) + consumed, bytes_to_consume);
    if (status != kStatusOk) {
      return status;
    }
    consumed += bytes_to_consume;
  }
  return kStatusOk;
}

Status StreamingChunkedCompressor::Final(size_t* compressed_size_out) {
  if (compressed_output_ == nullptr) {
    return kStatusErrBadState;
  }

  if (input_offset_ < input_len_) {
    // Final() was called before the entire input was processed.
    return kStatusErrBadState;
  }
  // There should not be any pending output frames.
  ZX_DEBUG_ASSERT(context_->current_output_frame_relative_pos_ > 0ul);

  Status status = writer_->Finalize();
  if (status == kStatusOk) {
    *compressed_size_out = compressed_output_offset_;
  }
  return status;
}

Status StreamingChunkedCompressor::AppendToFrame(const void* data, size_t len) {
  const size_t current_frame_start = fbl::round_down(input_offset_, params_.chunk_size);
  const size_t current_frame_end = fbl::min(current_frame_start + params_.chunk_size, input_len_);

  const size_t bytes_left_in_current_frame = current_frame_end - input_offset_;
  ZX_DEBUG_ASSERT(len <= bytes_left_in_current_frame);

  const bool will_finish_frame = bytes_left_in_current_frame == len;

  ZSTD_inBuffer in_buf;
  in_buf.src = data;
  in_buf.size = len;
  in_buf.pos = 0ul;

  // |out_buf| is set up to be relative to the current output frame we are processing.
  ZSTD_outBuffer out_buf;
  // dst is the start of the frame
  out_buf.dst = static_cast<uint8_t*>(compressed_output_) + context_->current_output_frame_start_;
  // size is the total number of bytes left in the output buffer
  out_buf.size = compressed_output_len_ - compressed_output_offset_;
  // pos is the progress past the start of the frame, so far.
  out_buf.pos = context_->current_output_frame_relative_pos_;

  size_t r = ZSTD_compressStream(context_->inner_, &out_buf, &in_buf);
  if (ZSTD_isError(r)) {
    FXL_LOG(ERROR) << "ZSTD_compressStream failed";
    return kStatusErrInternal;
  } else if (in_buf.pos < in_buf.size) {
    FXL_LOG(ERROR) << "Partial read";
    return kStatusErrInternal;
  }

  r = ZSTD_flushStream(context_->inner_, &out_buf);
  if (ZSTD_isError(r)) {
    FXL_LOG(ERROR) << "ZSTD_flushStream failed";
    return kStatusErrInternal;
  }
  if (will_finish_frame) {
    r = ZSTD_endStream(context_->inner_, &out_buf);
    if (ZSTD_isError(r)) {
      FXL_LOG(ERROR) << "ZSTD_endStream failed";
      return kStatusErrInternal;
    }
  }

  input_offset_ += len;
  compressed_output_offset_ += (out_buf.pos - context_->current_output_frame_relative_pos_);

  if (will_finish_frame) {
    // Case 1: The frame is finished. Write the seek table entry and advance to the next output
    // frame.
    SeekTableEntry entry;
    entry.decompressed_offset = current_frame_start;
    entry.decompressed_size = current_frame_end - current_frame_start;
    entry.compressed_offset = context_->current_output_frame_start_;
    entry.compressed_size = compressed_output_offset_ - context_->current_output_frame_start_;
    Status status = writer_->AddEntry(entry);
    if (status != kStatusOk) {
      return status;
    }

    if (progress_callback_) {
      (*progress_callback_)(input_offset_, input_len_, compressed_output_offset_);
    }

    context_->current_output_frame_start_ = compressed_output_offset_;
    context_->current_output_frame_relative_pos_ = 0ul;
  } else {
    // Case 2: The frame isn't complete yet. Mark our progress.
    context_->current_output_frame_relative_pos_ = out_buf.pos;
  }

  return kStatusOk;
}

}  // namespace chunked_compression
