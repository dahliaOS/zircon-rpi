// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chunked-decompressor.h"

#include <fbl/array.h>
#include <gtest/gtest.h>

namespace chunked_compression {
namespace {}  // namespace

TEST(ChunkedDecompressorTest, Decompress_EmptyArchive) {
  ChunkedArchiveHeader header;
  fbl::Array<uint8_t> buf(new uint8_t[header.SerializedHeaderSize()],
                          header.SerializedHeaderSize());
  ASSERT_EQ(header.Serialize(buf.get(), buf.size()), kStatusOk);

  fbl::Array<uint8_t> out_buf;
  size_t decompressed_size;
  ASSERT_EQ(
      ChunkedDecompressor::DecompressBytes(buf.get(), buf.size(), &out_buf, &decompressed_size),
      kStatusOk);
  EXPECT_EQ(decompressed_size, 0ul);
}

TEST(ChunkedDecompressorTest, Decompress_SingleFrame_Zeroes) {}

}  // namespace chunked_compression
