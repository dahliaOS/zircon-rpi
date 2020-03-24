// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zircon/assert.h>

#include <chunked-compression/streaming-chunked-compressor.h>
#include <fbl/algorithm.h>
#include <fbl/array.h>
#include <zxtest/zxtest.h>

namespace chunked_compression {
namespace {

void RandomFill(uint8_t* data, size_t len) {
  size_t off = 0;
  size_t rounded_len = fbl::round_down(len, sizeof(int));
  for (off = 0; off < rounded_len; off += sizeof(int)) {
    *reinterpret_cast<int*>(data + off) = rand();
  }
  ZX_ASSERT(off == rounded_len);
  for (; off < len; ++off) {
    data[off] = static_cast<uint8_t>(rand());
  }
}

void VerifyData(const uint8_t* compressed_data, size_t compressed_len, unsigned expected_frames,
                size_t expected_uncompressed_size) {
  ChunkedArchiveHeader archive;
  ASSERT_EQ(ChunkedArchiveHeader::Parse(compressed_data, compressed_len, &archive), kStatusOk);
  ASSERT_EQ(archive.SeekTable().size(), expected_frames);

  size_t decompressed_size_total = 0;
  // Include metadata in compressed size
  size_t compressed_size_total =
      kChunkArchiveSeekTableOffset + archive.SeekTable().size() * sizeof(SeekTableEntry);
  for (unsigned i = 0; i < archive.SeekTable().size(); ++i) {
    SeekTableEntry entry = archive.SeekTable()[i];
    decompressed_size_total += entry.decompressed_size;
    compressed_size_total += entry.compressed_size;
  }
  EXPECT_EQ(decompressed_size_total, expected_uncompressed_size);
  EXPECT_EQ(compressed_size_total, compressed_len);
}

}  // namespace

TEST(StreamingChunkedCompressorTest, ComputeOutputSizeLimit_Minimum) {
  StreamingChunkedCompressor compressor;
  // There should always be enough bytes for at least the metadata and one seek table entry.
  ASSERT_GE(compressor.ComputeOutputSizeLimit(1u),
            kChunkArchiveSeekTableOffset + sizeof(SeekTableEntry));
}

TEST(StreamingChunkedCompressorTest, Compress_Zeroes_Short) {
  size_t len = 8192ul;
  fbl::Array<uint8_t> data(new uint8_t[len], len);
  memset(data.get(), 0x00, len);

  StreamingChunkedCompressor compressor;

  size_t output_len = compressor.ComputeOutputSizeLimit(len);
  fbl::Array<uint8_t> compressed_data(new uint8_t[output_len], output_len);

  ASSERT_EQ(compressor.Init(len, compressed_data.get(), compressed_data.size()), kStatusOk);
  ASSERT_EQ(compressor.Update(data.get(), len), kStatusOk);
  size_t compressed_len;
  ASSERT_EQ(compressor.Final(&compressed_len), kStatusOk);

  VerifyData(compressed_data.get(), compressed_len, 1u, len);
}

TEST(StreamingChunkedCompressorTest, Compress_Random_Short) {
  size_t len = 8192ul;
  fbl::Array<uint8_t> data(new uint8_t[len], len);
  RandomFill(data.get(), len);

  StreamingChunkedCompressor compressor;

  size_t output_len = compressor.ComputeOutputSizeLimit(len);
  fbl::Array<uint8_t> compressed_data(new uint8_t[output_len], output_len);

  ASSERT_EQ(compressor.Init(len, compressed_data.get(), compressed_data.size()), kStatusOk);
  ASSERT_EQ(compressor.Update(data.get(), len), kStatusOk);
  size_t compressed_len;
  ASSERT_EQ(compressor.Final(&compressed_len), kStatusOk);

  VerifyData(compressed_data.get(), compressed_len, 1u, len);
}

TEST(StreamingChunkedCompressorTest, Compress_Zeroes_Long) {
  // 3 data frames, last one partial
  size_t len = (2 * CompressionParams::MinChunkSize()) + 42ul;
  fbl::Array<uint8_t> data(new uint8_t[len], len);
  memset(data.get(), 0x00, len);

  StreamingChunkedCompressor compressor;

  size_t output_len = compressor.ComputeOutputSizeLimit(len);
  fbl::Array<uint8_t> compressed_data(new uint8_t[output_len], output_len);

  ASSERT_EQ(compressor.Init(len, compressed_data.get(), compressed_data.size()), kStatusOk);
  ASSERT_EQ(compressor.Update(data.get(), len), kStatusOk);
  size_t compressed_len;
  ASSERT_EQ(compressor.Final(&compressed_len), kStatusOk);

  VerifyData(compressed_data.get(), compressed_len, 3u, len);
}

TEST(StreamingChunkedCompressorTest, Compress_Zeroes_Long_RandomUpdateSizes) {
  // 3 data frames, last one partial
  size_t len = (2 * CompressionParams::MinChunkSize()) + 42ul;
  fbl::Array<uint8_t> data(new uint8_t[len], len);
  memset(data.get(), 0x00, len);

  StreamingChunkedCompressor compressor;

  size_t output_len = compressor.ComputeOutputSizeLimit(len);
  fbl::Array<uint8_t> compressed_data(new uint8_t[output_len], output_len);

  ASSERT_EQ(compressor.Init(len, compressed_data.get(), compressed_data.size()), kStatusOk);

  size_t off = 0;
  while (off < len) {
    constexpr size_t kMaxChunkSize = 8192;
    size_t chunk = (rand() % kMaxChunkSize) + 1;
    if (chunk > len - off) {
      chunk = len - off;
    }
    ASSERT_EQ(compressor.Update(data.get() + off, chunk), kStatusOk);
    off += chunk;
  }
  size_t compressed_len;

  ASSERT_EQ(compressor.Final(&compressed_len), kStatusOk);

  VerifyData(compressed_data.get(), compressed_len, 3u, len);
}

TEST(StreamingChunkedCompressorTest, Compress_Random_Long) {
  // 3 data frames, last one partial
  size_t len = (2 * CompressionParams::MinChunkSize()) + 42ul;
  fbl::Array<uint8_t> data(new uint8_t[len], len);
  RandomFill(data.get(), len);

  StreamingChunkedCompressor compressor;

  size_t output_len = compressor.ComputeOutputSizeLimit(len);
  fbl::Array<uint8_t> compressed_data(new uint8_t[output_len], output_len);

  ASSERT_EQ(compressor.Init(len, compressed_data.get(), compressed_data.size()), kStatusOk);
  ASSERT_EQ(compressor.Update(data.get(), len), kStatusOk);
  size_t compressed_len;
  ASSERT_EQ(compressor.Final(&compressed_len), kStatusOk);

  VerifyData(compressed_data.get(), compressed_len, 3u, len);
}

TEST(StreamingChunkedCompressorTest, Compress_Random_Long_RandomUpdateSizes) {
  // 3 data frames, last one partial
  size_t len = (2 * CompressionParams::MinChunkSize()) + 42ul;
  fbl::Array<uint8_t> data(new uint8_t[len], len);
  RandomFill(data.get(), len);

  StreamingChunkedCompressor compressor;

  size_t output_len = compressor.ComputeOutputSizeLimit(len);
  fbl::Array<uint8_t> compressed_data(new uint8_t[output_len], output_len);

  ASSERT_EQ(compressor.Init(len, compressed_data.get(), compressed_data.size()), kStatusOk);

  size_t off = 0;
  while (off < len) {
    constexpr size_t kMaxChunkSize = 8192;
    size_t chunk = (rand() % kMaxChunkSize) + 1;
    if (chunk > len - off) {
      chunk = len - off;
    }
    ASSERT_EQ(compressor.Update(data.get() + off, chunk), kStatusOk);
    off += chunk;
  }
  size_t compressed_len;

  ASSERT_EQ(compressor.Final(&compressed_len), kStatusOk);

  VerifyData(compressed_data.get(), compressed_len, 3u, len);
}

TEST(StreamingChunkedCompressorTest, Compress_ReuseCompressor) {
  StreamingChunkedCompressor compressor;

  {
    size_t len = 8192ul;
    fbl::Array<uint8_t> data(new uint8_t[len], len);
    memset(data.get(), 0x00, len);

    size_t compressed_limit = compressor.ComputeOutputSizeLimit(len);
    fbl::Array<uint8_t> compressed_data(new uint8_t[compressed_limit], compressed_limit);
    ASSERT_EQ(compressor.Init(len, compressed_data.get(), compressed_data.size()), kStatusOk);
    ASSERT_EQ(compressor.Update(data.get(), len), kStatusOk);
    size_t compressed_len;
    ASSERT_EQ(compressor.Final(&compressed_len), kStatusOk);

    VerifyData(compressed_data.get(), compressed_len, 1u, len);
  }
  {
    size_t len = 8192ul;
    fbl::Array<uint8_t> data(new uint8_t[len], len);
    // Set with different input data.
    memset(data.get(), 0xac, len);

    size_t compressed_limit = compressor.ComputeOutputSizeLimit(len);
    fbl::Array<uint8_t> compressed_data(new uint8_t[compressed_limit], compressed_limit);
    ASSERT_EQ(compressor.Init(len, compressed_data.get(), compressed_data.size()), kStatusOk);
    ASSERT_EQ(compressor.Update(data.get(), len), kStatusOk);
    size_t compressed_len;
    ASSERT_EQ(compressor.Final(&compressed_len), kStatusOk);

    VerifyData(compressed_data.get(), compressed_len, 1u, len);
  }
}
}  // namespace chunked_compression
