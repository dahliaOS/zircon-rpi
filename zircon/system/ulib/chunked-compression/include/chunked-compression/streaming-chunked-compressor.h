// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHUNKED_COMPRESSION_STREAMING_CHUNKED_COMPRESSOR_H_
#define CHUNKED_COMPRESSION_STREAMING_CHUNKED_COMPRESSOR_H_

#include <memory>
#include <optional>

#include <fbl/array.h>
#include <fbl/function.h>
#include <fbl/macros.h>

#include "chunked-archive.h"
#include "chunked-compressor.h"
#include "status.h"

namespace chunked_compression {

class StreamingChunkedCompressor {
 public:
  StreamingChunkedCompressor();
  explicit StreamingChunkedCompressor(CompressionParams params);
  ~StreamingChunkedCompressor();
  StreamingChunkedCompressor(StreamingChunkedCompressor&& o);
  StreamingChunkedCompressor& operator=(StreamingChunkedCompressor&& o);
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(StreamingChunkedCompressor);

  // Returns the minimum size that a buffer must be to hold the result of compressing |len| bytes.
  size_t ComputeOutputSizeLimit(size_t len);

  Status Init(size_t data_len, void* dst, size_t dst_len);
  Status Update(const void* data, size_t len);
  Status Final(size_t* compressed_size_out);

  // Registers |callback| to be invoked after each frame is complete.
  using ProgressFn =
      fbl::Function<void(size_t bytes_read, size_t bytes_total, size_t bytes_written)>;
  void SetProgressCallback(ProgressFn callback) { progress_callback_ = std::move(callback); }

 private:
  // Must be called before each new frame is written to, and can only be called when |input_offset_|
  // falls on a frame boundary.
  Status StartFrame();
  // Must be called after each frame is completed.
  Status EndFrame(size_t uncompressed_frame_start, size_t uncompressed_frame_len);
  // Appends |len| bytes to the current frame. |len| must be less than the expected size of the
  // frame.
  // Calls EndFrame if the frame was completed by this data, and then calls StartFrame if there is
  // still more data expected in the input stream.
  Status AppendToFrame(const void* data, size_t len);
  void MoveFrom(StreamingChunkedCompressor&& o);
  uint8_t* compressed_output_;
  size_t compressed_output_len_;
  size_t compressed_output_offset_;

  size_t input_len_;
  size_t input_offset_;

  std::unique_ptr<ChunkedArchiveWriter> writer_;

  std::optional<ProgressFn> progress_callback_;

  CompressionParams params_;

  struct CompressionContext;
  std::unique_ptr<CompressionContext> context_;
};

}  // namespace chunked_compression

#endif  // CHUNKED_COMPRESSION_STREAMING_CHUNKED_COMPRESSOR_H_
