// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_CHUNKED_COMPRESSION_CHUNKED_ARCHIVE_H_
#define SRC_STORAGE_CHUNKED_COMPRESSION_CHUNKED_ARCHIVE_H_

#include <optional>

#include <fbl/algorithm.h>
#include <fbl/array.h>

#include "src/lib/fxl/macros.h"
#include "src/storage/chunked-compression/status.h"

namespace chunked_compression {

using ArchiveMagicType = uint64_t;
using ChunkCountType = uint32_t;

constexpr ArchiveMagicType kChunkedCompressionArchiveMagic = 0x60427041'62407140;

constexpr size_t kChunkArchiveMagicOffset = 0ul;
constexpr size_t kChunkArchiveReservedOffset = 8ul;
constexpr size_t kChunkArchiveNumChunksOffset = 12ul;
constexpr size_t kChunkArchiveSeekTableOffset = 16ul;

static_assert(kChunkArchiveMagicOffset == 0ul, "Breaking change to archive format");
static_assert(kChunkArchiveReservedOffset == kChunkArchiveMagicOffset + sizeof(ArchiveMagicType),
              "Breaking change to archive format");
static_assert(kChunkArchiveNumChunksOffset == kChunkArchiveReservedOffset + sizeof(uint32_t),
              "Breaking change to archive format");
static_assert(kChunkArchiveSeekTableOffset == kChunkArchiveNumChunksOffset + sizeof(ChunkCountType),
              "Breaking change to archive format");

// A single entry into the seek table. Describes where an extent of decompressed
// data lives in the compressed space.
struct SeekTableEntry {
  uint64_t decompressed_offset;
  uint64_t decompressed_size;
  uint64_t compressed_offset;
  uint64_t compressed_size;
};

// A parsed view of a chunked archive.
class ChunkedArchiveHeader {
 public:
  // Creates an empty archive with no seek table entries.
  ChunkedArchiveHeader() = default;
  ~ChunkedArchiveHeader() = default;
  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(ChunkedArchiveHeader);

  // Validates that |data| is a valid chunked archive header and fills |out| with a copy of its
  // contents.
  // |len| must be at least long enough to include the entire header; any actual compressed frames
  // contained in |data| will not be accessed.
  static Status Parse(const void* data, size_t len, ChunkedArchiveHeader* out);

  // Returns a reference to the seek table of the archive.
  const fbl::Array<SeekTableEntry>& SeekTable() const { return seek_table_; }

  // Returns the size of the compressed archive.
  size_t CompressedSize() const;

  // Returns the size of the serialized header (i.e. everything but the actual compressed frames).
  size_t SerializedHeaderSize() const;

  // Returns the expected size of the archive after decompression.
  size_t DecompressedSize() const;

  // Serializes the header and writes it to |dst|.
  // |dst_len| must be big enough for the entire header, including magic and seek table size.
  Status Serialize(void* dst, size_t dst_len) const;

  // Lookup functions to find the entry in the seek table which covers |offset| in either the
  // compressed or decompressed space.
  // Returns the index into |SeekTable()| where the entry is stored, or std::nullopt if the
  // offset is out of bounds.
  std::optional<unsigned> EntryForCompressedOffset(size_t offset) const;
  std::optional<unsigned> EntryForDecompressedOffset(size_t offset) const;

  friend class ChunkedArchiveWriter;

 private:
  static Status CheckMagic(const uint8_t* data, size_t len);
  static Status GetNumChunks(const uint8_t* data, size_t len, ChunkCountType* num_chunks_out);
  static Status ParseSeekTable(const uint8_t* data, size_t len,
                               fbl::Array<SeekTableEntry>* seek_table_out);
  static Status CheckSeekTableEntry(const SeekTableEntry& entry);
  static bool EntriesOverlap(const SeekTableEntry& a, const SeekTableEntry& b);

  fbl::Array<SeekTableEntry> seek_table_;
};

// ChunkedArchiveWriter writes chunked archive headers to a target buffer.
class ChunkedArchiveWriter {
 public:
  ChunkedArchiveWriter(void* dst, size_t dst_len, size_t num_frames);
  ChunkedArchiveWriter() = delete;
  ~ChunkedArchiveWriter() = default;
  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(ChunkedArchiveWriter);

  // Computes the number of frames which will be used to compress a |size|-byte input.
  static size_t NumFramesForDataSize(size_t size, size_t chunk_size) {
    return fbl::round_up(size, chunk_size) / chunk_size;
  }

  // Computes the size of the metadata header necessary for an archive with |num_frames|.
  static size_t MetadataSizeForNumFrames(size_t num_frames) {
    return kChunkArchiveSeekTableOffset + (num_frames * sizeof(SeekTableEntry));
  }

  // Adds a copy of |entry| to the seek table.
  // Returns an error if |entry| is invalid, overlaps an existing entry, or if the table is already
  // full.
  Status AddEntry(const SeekTableEntry& entry);

  // Finishes writing the header out to the target buffer.
  //
  // Returns an error if the header was not fully initialized (i.e. not every seek table entry
  // was filled).
  //
  // The target buffer is in an undefined state before Finalize() is called, and should not be
  // serialized until Finalize() returns successfully.
  //
  // The ChunkedArchiveWriter is in an undefined state after Finalize() returns, regardless of
  // whether Finalize() succeeded or not.
  Status Finalize();

 private:
  uint8_t* const dst_;
  SeekTableEntry* seek_table_;
  unsigned current_frame_ = 0;
  unsigned num_frames_;
};

}  // namespace chunked_compression

#endif  // SRC_STORAGE_CHUNKED_COMPRESSION_CHUNKED_ARCHIVE_H_
