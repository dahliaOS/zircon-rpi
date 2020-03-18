// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fbl/unique_fd.h>

#include "src/lib/fxl/command_line.h"
#include "src/storage/chunked-compression/chunked-compressor.h"
#include "src/storage/chunked-compression/chunked-decompressor.h"
#include "src/storage/chunked-compression/status.h"

namespace {

using chunked_compression::ChunkedArchiveHeader;
using chunked_compression::ChunkedCompressor;
using chunked_compression::ChunkedDecompressor;
using chunked_compression::CompressionParams;

constexpr const char kAnsiUpLine[] = "\33[A";
constexpr const char kAnsiClearLine[] = "\33[2K\r";

void usage(const char* fname) {
  fprintf(stderr, "Usage: %s [--level #] [d | c] source dest\n", fname);
  fprintf(stderr,
          "\
  c: Compress source, writing to dest.\n\
  d: Decompress source, writing to dest.\n\
  --level #: Compression level\n");
}

int OpenAndMapForWriting(const char* file, size_t write_size, uint8_t** out_write_buf,
                         fbl::unique_fd* out_fd) {
  fbl::unique_fd fd(open(file, O_RDWR | O_CREAT | O_TRUNC));
  if (!fd.is_valid()) {
    fprintf(stderr, "Failed to open '%s': %s\n", file, strerror(errno));
    return 1;
  }
  if (ftruncate(fd.get(), write_size)) {
    fprintf(stderr, "Failed to truncate '%s': %s\n", file, strerror(errno));
    return 1;
  }

  void* data = nullptr;
  if (write_size > 0) {
    data = mmap(NULL, write_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (data == MAP_FAILED) {
      fprintf(stderr, "mmap failed: %s\n", strerror(errno));
      return 1;
    }
  }

  *out_write_buf = static_cast<uint8_t*>(data);
  *out_fd = std::move(fd);

  return 0;
}

int OpenAndMapForReading(const char* file, fbl::unique_fd* out_fd, const uint8_t** out_buf,
                         size_t* out_size) {
  fbl::unique_fd fd(open(file, O_RDONLY));
  if (!fd.is_valid()) {
    fprintf(stderr, "Failed to open '%s'.\n", file);
    return 1;
  }
  size_t size;
  struct stat info;
  if (fstat(fd.get(), &info) < 0) {
    fprintf(stderr, "stat(%s) failed: %s\n", file, strerror(errno));
    return 1;
  }
  if (!S_ISREG(info.st_mode)) {
    fprintf(stderr, "%s is not a regular file\n", file);
    return 1;
  }
  size = info.st_size;

  void* data = nullptr;
  if (size > 0) {
    data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd.get(), 0);
    if (data == MAP_FAILED) {
      fprintf(stderr, "mmap failed: %s\n", strerror(errno));
      return 1;
    }
  }

  *out_fd = std::move(fd);
  *out_buf = static_cast<uint8_t*>(data);
  *out_size = size;

  return 0;
}

int Compress(const uint8_t* src, size_t sz, const char* dst_file, int level) {
  CompressionParams params;
  params.compression_level = level;
  params.chunk_size = CompressionParams::ChunkSizeForInputSize(sz);
  ChunkedCompressor compressor(params);
  size_t output_limit = compressor.ComputeOutputSizeLimit(sz);

  fbl::unique_fd dst_fd;
  uint8_t* write_buf;
  if (OpenAndMapForWriting(dst_file, output_limit, &write_buf, &dst_fd)) {
    return 1;
  }

  const int refresh_hz = 60;
  const std::chrono::duration refresh_duration = std::chrono::seconds(1) / refresh_hz;
  std::chrono::time_point last_report = std::chrono::steady_clock::time_point::min();
  printf("\n");
  compressor.SetProgressCallback([&](size_t bytes_read, size_t bytes_total, size_t bytes_written) {
    auto now = std::chrono::steady_clock::now();
    if (last_report + refresh_duration <= now) {
      last_report = now;
      printf("%s%s", kAnsiUpLine, kAnsiClearLine);
      printf("%2.0f%% (%lu bytes written)\n",
             static_cast<double>(bytes_read) / static_cast<double>(bytes_total) * 100,
             bytes_written);
      fflush(stdout);
    }
  });

  size_t compressed_size;
  if (compressor.Compress(src, sz, write_buf, output_limit, &compressed_size) !=
      chunked_compression::kStatusOk) {
    return 1;
  }

  printf("%s%s", kAnsiUpLine, kAnsiClearLine);
  printf("Wrote %lu bytes (%2.0f%% compression)\n", compressed_size,
         static_cast<double>(compressed_size) / static_cast<double>(sz) * 100);
  fflush(stdout);

  ftruncate(dst_fd.get(), compressed_size);
  return 0;
}

int Decompress(const uint8_t* src, size_t sz, const char* dst_file) {
  ChunkedArchiveHeader header;
  if ((ChunkedArchiveHeader::Parse(src, sz, &header)) != chunked_compression::kStatusOk) {
    fprintf(stderr, "Failed to parse input file\n");
    return 1;
  }
  size_t output_size = ChunkedDecompressor::ComputeOutputSize(header);

  fbl::unique_fd dst_fd;
  uint8_t* write_buf;
  if (OpenAndMapForWriting(dst_file, output_size, &write_buf, &dst_fd)) {
    return 1;
  }

  ChunkedDecompressor decompressor;
  size_t bytes_written;
  if (decompressor.Decompress(header, src, sz, write_buf, output_size, &bytes_written) !=
      chunked_compression::kStatusOk) {
    return 1;
  }

  printf("Wrote %lu bytes (%2.0f%% compression)\n", bytes_written,
         static_cast<double>(sz) / static_cast<double>(bytes_written) * 100);
  return 0;
}

}  // namespace

int main(int argc, const char** argv) {
  if (argc < 4) {
    usage(argv[0]);
    return 1;
  }
  const fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  const auto& positional_args = command_line.positional_args();

  if (positional_args.size() < 3) {
    usage(argv[0]);
    return 1;
  }
  const std::string& mode_str = positional_args[0];
  const std::string& input_file = positional_args[1];
  const std::string& output_file = positional_args[2];

  enum Mode {
    COMPRESS,
    DECOMPRESS,
    UNKNOWN,
  };
  Mode mode = Mode::UNKNOWN;
  if (mode_str == "d") {
    mode = Mode::DECOMPRESS;
  } else if (mode_str == "c") {
    mode = Mode::COMPRESS;
  } else {
    fprintf(stderr, "Invalid mode (should be 'd' or 'c').\n");
    usage(argv[0]);
    return 1;
  }

  int level = CompressionParams::DefaultCompressionLevel();
  if (command_line.HasOption("level")) {
    if (mode == Mode::DECOMPRESS) {
      printf("Ignoring --level flag for decompression\n");
    } else {
      std::string level_str;
      ZX_ASSERT(command_line.GetOptionValue("level", &level_str));
      char* endp;
      long val = strtol(level_str.c_str(), &endp, 10);
      if (endp != level_str.c_str() + level_str.length()) {
        usage(argv[0]);
        return 1;
      } else if (level < CompressionParams::MinCompressionLevel() ||
                 level > CompressionParams::MaxCompressionLevel()) {
        fprintf(stderr, "Invalid level, should be in range %d <= level <= %d\n",
                CompressionParams::MinCompressionLevel(), CompressionParams::MaxCompressionLevel());
        return 1;
      }
      level = static_cast<int>(val);
    }
  }

  fbl::unique_fd src_fd;
  const uint8_t* src_data;
  size_t src_size;
  if (OpenAndMapForReading(input_file.c_str(), &src_fd, &src_data, &src_size)) {
    return 1;
  }

  return mode == Mode::COMPRESS ? Compress(src_data, src_size, output_file.c_str(), level)
                                : Decompress(src_data, src_size, output_file.c_str());
}
