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
#include "src/storage/chunked-compression/streaming-chunked-compressor.h"

namespace {

using chunked_compression::ChunkedArchiveHeader;
using chunked_compression::ChunkedCompressor;
using chunked_compression::ChunkedDecompressor;
using chunked_compression::CompressionParams;
using chunked_compression::StreamingChunkedCompressor;

constexpr const char kAnsiUpLine[] = "\33[A";
constexpr const char kAnsiClearLine[] = "\33[2K\r";

class ProgressWriter {
 public:
  explicit ProgressWriter(int refresh_hz = 60) : refresh_hz_(refresh_hz) {
    last_report_ = std::chrono::steady_clock::time_point::min();
  }

  void Update(const char* fmt, ...) {
    auto now = std::chrono::steady_clock::now();
    if (now < last_report_ + refresh_duration()) {
      return;
    }
    last_report_ = now;
    printf("%s%s", kAnsiUpLine, kAnsiClearLine);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
  }

  void Final(const char* fmt, ...) {
    printf("%s%s", kAnsiUpLine, kAnsiClearLine);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
  }

  std::chrono::steady_clock::duration refresh_duration() const {
    return std::chrono::seconds(1) / refresh_hz_;
  }

 private:
  std::chrono::steady_clock::time_point last_report_;
  int refresh_hz_;
};

void usage(const char* fname) {
  fprintf(stderr, "Usage: %s [--level #] [--streaming] [d | c] source dest\n", fname);
  fprintf(stderr,
          "\
  c: Compress source, writing to dest.\n\
  d: Decompress source, writing to dest.\n\
  --streaming: Use streaming compression\n\
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

size_t GetSize(const char* file) {
  struct stat info;
  if (stat(file, &info) < 0) {
    fprintf(stderr, "stat(%s) failed: %s\n", file, strerror(errno));
    return 0;
  }
  if (!S_ISREG(info.st_mode)) {
    fprintf(stderr, "%s is not a regular file\n", file);
    return 0;
  }
  return info.st_size;
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

  ProgressWriter progress;
  compressor.SetProgressCallback([&](size_t bytes_read, size_t bytes_total, size_t bytes_written) {
    progress.Update("%2.0f%% (%lu bytes written)\n",
                    static_cast<double>(bytes_read) / static_cast<double>(bytes_total) * 100,
                    bytes_written);
  });

  size_t compressed_size;
  if (compressor.Compress(src, sz, write_buf, output_limit, &compressed_size) !=
      chunked_compression::kStatusOk) {
    return 1;
  }

  progress.Final("Wrote %lu bytes (%2.0f%% compression)\n", compressed_size,
                 static_cast<double>(compressed_size) / static_cast<double>(sz) * 100);

  ftruncate(dst_fd.get(), compressed_size);
  return 0;
}

int CompressStream(fbl::unique_fd src_fd, size_t sz, const char* dst_file, int level) {
  CompressionParams params;
  params.compression_level = level;
  params.chunk_size = CompressionParams::ChunkSizeForInputSize(sz);
  StreamingChunkedCompressor compressor(params);
  size_t output_limit = compressor.ComputeOutputSizeLimit(sz);

  fbl::unique_fd dst_fd;
  uint8_t* write_buf;
  if (OpenAndMapForWriting(dst_file, output_limit, &write_buf, &dst_fd)) {
    return 1;
  }

  if (compressor.Init(sz, write_buf, output_limit) != chunked_compression::kStatusOk) {
    fprintf(stderr, "Final failed\n");
    return 1;
  }

  ProgressWriter progress;
  compressor.SetProgressCallback([&](size_t bytes_read, size_t bytes_total, size_t bytes_written) {
    progress.Update("%2.0f%% (%lu bytes written)\n",
                    static_cast<double>(bytes_read) / static_cast<double>(bytes_total) * 100,
                    bytes_written);
  });

  FILE* in = fdopen(src_fd.get(), "r");
  ZX_ASSERT(in != nullptr);
  const size_t chunk_size = 8192;
  uint8_t buf[chunk_size];
  size_t bytes_read = 0;
  for (size_t off = 0; off < sz; off += chunk_size) {
    size_t r = fread(buf, sizeof(uint8_t), chunk_size, in);
    if (r == 0) {
      int err = ferror(in);
      if (err) {
        fprintf(stderr, "fread failed: %d\n", err);
        return err;
      }
      break;
    }
    if (compressor.Update(buf, r) != chunked_compression::kStatusOk) {
      fprintf(stderr, "Update failed\n");
      return 1;
    }
    bytes_read += r;
  }
  if (bytes_read < sz) {
    fprintf(stderr, "Only read %lu bytes (expected %lu)\n", bytes_read, sz);
  }

  size_t compressed_size;
  if (compressor.Final(&compressed_size) != chunked_compression::kStatusOk) {
    fprintf(stderr, "Final failed\n");
    return 1;
  }

  progress.Final("Wrote %lu bytes (%2.0f%% compression)\n", compressed_size,
                 static_cast<double>(compressed_size) / static_cast<double>(sz) * 100);

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
  bool stream = command_line.HasOption("streaming");
  if (stream) {
    if (mode == Mode::DECOMPRESS) {
      printf("Ignoring --streaming flag for decompression\n");
    } else {
      fbl::unique_fd fd(open(input_file.c_str(), O_RDONLY));
      if (!fd.is_valid()) {
        fprintf(stderr, "Failed to open '%s'.\n", input_file.c_str());
        return 1;
      }
      return CompressStream(std::move(fd), GetSize(input_file.c_str()), output_file.c_str(), level);
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
