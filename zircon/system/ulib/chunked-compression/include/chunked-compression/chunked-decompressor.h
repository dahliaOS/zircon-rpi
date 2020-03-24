// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHUNKED_COMPRESSION_CHUNKED_DECOMPRESSOR_H_
#define CHUNKED_COMPRESSION_CHUNKED_DECOMPRESSOR_H_

#include <memory>

#include <fbl/array.h>
#include <fbl/function.h>
#include <fbl/macros.h>

#include "chunked-archive.h"
#include "status.h"

namespace chunked_compression {

class ChunkedDecompressor {
 public:
  ChunkedDecompressor();
  ~ChunkedDecompressor();
  ChunkedDecompressor(ChunkedDecompressor&& o) = default;
  ChunkedDecompressor& operator=(ChunkedDecompressor&& o) = default;
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ChunkedDecompressor);

  // Convenience method to do a one-shot decompression of |data|, returning an allocated buffer
  // containing the decompressed bytes.
  static Status DecompressBytes(const void* data, size_t len, fbl::Array<uint8_t>* decompressed_out,
                                size_t* bytes_written_out);

  // Returns the minimum size that a buffer must be to hold the result of decompressing the archive
  // described by |header|.
  static size_t ComputeOutputSize(const ChunkedArchiveHeader& header) {
    return header.DecompressedSize();
  }

  // Reads the decompressed archive described by |header| from |data|, and writes the decompressed
  // data to |dst|.
  //
  // |data| should include the full archive contents, including the header itself. The header is
  // not validated (having already been validated during construction of |header|).
  // |dst_len| must be at least |ComputeOutputSize(header)| bytes long.
  //
  // Returns the number of decompressed bytes written in |bytes_written_out|.
  Status Decompress(const ChunkedArchiveHeader& header, const void* data, size_t len, void* dst,
                    size_t dst_len, size_t* bytes_written_out);

  // Reads the |frame_num|'th frame of the decompressed archive described by |header| from |data|,
  // and writes the decompressed frame to |dst|.
  //
  // |frame_data| should start at the frame's first byte, and |frame_len| must be big enough to span
  // the entire frame.
  // |dst_len| must be at least as big as |header.SeekTable()[frame_num].decompressed_size|.
  //
  // Returns the number of decompressed bytes written in |bytes_written_out|.
  Status DecompressFrame(const ChunkedArchiveHeader& header, unsigned frame_num,
                         const void* frame_data, size_t frame_len, void* dst, size_t dst_len,
                         size_t* bytes_written_out);

 private:
  struct DecompressionContext;
  std::unique_ptr<DecompressionContext> context_;
};

}  // namespace chunked_compression

#endif  // CHUNKED_COMPRESSION_CHUNKED_DECOMPRESSOR_H_
