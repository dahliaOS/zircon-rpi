// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chunked-archive.h"

#include <zircon/compiler.h>

#include <fbl/array.h>

#include "src/lib/fxl/logging.h"
#include "src/storage/chunked-compression/status.h"

namespace chunked_compression {
namespace {

// Returns |true| if [a_start, a_start + a_len) overlaps [b_start, b_start + b_len).
// Assumes that neither addition described above will overflow.
bool RangeOverlaps(size_t a_start, size_t a_len, size_t b_start, size_t b_len) {
  if (a_len == 0 || b_len == 0) {
    return false;
  }
  size_t a_end, b_end;
  ZX_ASSERT(!add_overflow(a_start, a_len, &a_end));
  ZX_ASSERT(!add_overflow(b_start, b_len, &b_end));
  if (a_start < b_start) {
    return a_end > b_start;
  } else if (a_start == b_start) {
    return true;
  } else {
    return b_end > a_start;
  }
}

}  // namespace

// ChunkedArchiveHeader

Status ChunkedArchiveHeader::Parse(const void* data, size_t len, ChunkedArchiveHeader* out) {
  Status status;
  if ((status = CheckMagic(static_cast<const uint8_t*>(data), len)) != kStatusOk) {
    return status;
  }
  fbl::Array<SeekTableEntry> seek_table;
  if ((status = ParseSeekTable(static_cast<const uint8_t*>(data), len, &seek_table)) != kStatusOk) {
    return status;
  }

  out->seek_table_ = std::move(seek_table);

  return kStatusOk;
}

size_t ChunkedArchiveHeader::CompressedSize() const {
  // Include all of the metadata.
  size_t sz = SerializedHeaderSize();
  for (unsigned i = 0; i < seek_table_.size(); ++i) {
    sz += seek_table_[i].compressed_size;
  }
  return sz;
}

size_t ChunkedArchiveHeader::SerializedHeaderSize() const {
  return kChunkArchiveSeekTableOffset + (seek_table_.size() * sizeof(SeekTableEntry));
}

size_t ChunkedArchiveHeader::DecompressedSize() const {
  size_t sz = 0;
  for (unsigned i = 0; i < seek_table_.size(); ++i) {
    sz += seek_table_[i].decompressed_size;
  }
  return sz;
}

Status ChunkedArchiveHeader::Serialize(void* dst, size_t dst_len) const {
  if (dst_len < SerializedHeaderSize()) {
    return kStatusErrBufferTooSmall;
  }
  uint8_t* target = static_cast<uint8_t*>(dst);

  reinterpret_cast<ArchiveMagicType*>(target + kChunkArchiveMagicOffset)[0] =
      kChunkedCompressionArchiveMagic;
  reinterpret_cast<ChunkCountType*>(target + kChunkArchiveNumChunksOffset)[0] = seek_table_.size();
  SeekTableEntry* target_seek_table =
      reinterpret_cast<SeekTableEntry*>(target + kChunkArchiveSeekTableOffset);
  for (unsigned i = 0; i < seek_table_.size(); ++i) {
    target_seek_table[i] = seek_table_[i];
  }

  return kStatusOk;
}

Status ChunkedArchiveHeader::CheckMagic(const uint8_t* data, size_t len) {
  if (len < sizeof(kChunkedCompressionArchiveMagic)) {
    return kStatusErrIoDataIntegrity;
  }
  const ArchiveMagicType& magic =
      reinterpret_cast<const ArchiveMagicType*>(data + kChunkArchiveMagicOffset)[0];
  return magic == kChunkedCompressionArchiveMagic ? kStatusOk : kStatusErrIoDataIntegrity;
}

Status ChunkedArchiveHeader::GetNumChunks(const uint8_t* data, size_t len,
                                          ChunkCountType* num_chunks_out) {
  if (len < kChunkArchiveNumChunksOffset + sizeof(ChunkCountType)) {
    return kStatusErrIoDataIntegrity;
  }
  *num_chunks_out = reinterpret_cast<const ChunkCountType*>(data + kChunkArchiveNumChunksOffset)[0];
  return kStatusOk;
}

Status ChunkedArchiveHeader::ParseSeekTable(const uint8_t* data, size_t len,
                                            fbl::Array<SeekTableEntry>* seek_table_out) {
  ChunkCountType num_chunks;
  Status status = GetNumChunks(data, len, &num_chunks);
  if (status != kStatusOk) {
    return status;
  } else if (len < kChunkArchiveSeekTableOffset + (num_chunks * sizeof(SeekTableEntry))) {
    FXL_LOG(ERROR) << "Invalid archive. Header too small for seek table size";
    return kStatusErrIoDataIntegrity;
  }

  seek_table_out->reset(new SeekTableEntry[num_chunks], num_chunks);

  const SeekTableEntry* entries =
      reinterpret_cast<const SeekTableEntry*>(data + kChunkArchiveSeekTableOffset);
  for (unsigned i = 0; i < num_chunks; ++i) {
    // Validate each entry separately before calling EntriesOverlap pair-wise so we can assume
    // both entries are valid in EntriesOverlap.
    if ((status = CheckSeekTableEntry(entries[i])) != kStatusOk) {
      FXL_LOG(ERROR) << "Invalid archive. Bad seek table entry " << i;
      return status;
    }
  }
  for (unsigned i = 0; i < num_chunks; ++i) {
    for (unsigned j = i + 1; j < num_chunks; ++j) {
      if (EntriesOverlap(entries[i], entries[j])) {
        FXL_LOG(ERROR) << "Invalid archive. Chunks " << i << " and " << j << " overlap.";
        return kStatusErrIoDataIntegrity;
      }
    }
    (*seek_table_out)[i] = entries[i];
  }
  return kStatusOk;
}

Status ChunkedArchiveHeader::CheckSeekTableEntry(const SeekTableEntry& entry) {
  if (entry.compressed_size == 0 || entry.decompressed_size == 0) {
    return kStatusErrIoDataIntegrity;
  }
  __UNUSED uint64_t compressed_end;
  if (add_overflow(entry.compressed_offset, entry.compressed_size, &compressed_end)) {
    return kStatusErrIoDataIntegrity;
  }
  __UNUSED uint64_t decompressed_end;
  if (add_overflow(entry.decompressed_offset, entry.decompressed_size, &decompressed_end)) {
    return kStatusErrIoDataIntegrity;
  }
  return kStatusOk;
}

bool ChunkedArchiveHeader::EntriesOverlap(const SeekTableEntry& a, const SeekTableEntry& b) {
  return RangeOverlaps(a.compressed_offset, a.compressed_size, b.compressed_offset,
                       b.compressed_size) ||
         RangeOverlaps(a.decompressed_offset, a.decompressed_size, b.decompressed_offset,
                       b.decompressed_size);
}

// ChunkedArchiveWriter

ChunkedArchiveWriter::ChunkedArchiveWriter(void* dst, size_t dst_len, size_t num_frames)
    : dst_(static_cast<uint8_t*>(dst)), num_frames_(num_frames) {
  ZX_ASSERT(dst_len >= kChunkArchiveSeekTableOffset + (num_frames * sizeof(SeekTableEntry)));
  seek_table_ = reinterpret_cast<SeekTableEntry* const>(dst_ + kChunkArchiveSeekTableOffset);
}

Status ChunkedArchiveWriter::AddEntry(const SeekTableEntry& entry) {
  if (current_frame_ == num_frames_) {
    return kStatusErrBadState;
  }
  Status status = ChunkedArchiveHeader::CheckSeekTableEntry(entry);
  if (status != kStatusOk) {
    return status;
  }
  for (unsigned i = 0; i < current_frame_; ++i) {
    if (ChunkedArchiveHeader::EntriesOverlap(entry, seek_table_[i])) {
      return kStatusErrInvalidArgs;
    }
  }
  seek_table_[current_frame_] = entry;
  ++current_frame_;
  return kStatusOk;
}

Status ChunkedArchiveWriter::Finalize() {
  if (num_frames_ < current_frame_) {
    return kStatusErrBadState;
  }
  reinterpret_cast<ArchiveMagicType*>(dst_ + kChunkArchiveMagicOffset)[0] =
      kChunkedCompressionArchiveMagic;
  reinterpret_cast<uint32_t*>(dst_ + kChunkArchiveReservedOffset)[0] = 0u;
  reinterpret_cast<ChunkCountType*>(dst_ + kChunkArchiveNumChunksOffset)[0] = num_frames_;
  return kStatusOk;
}

}  // namespace chunked_compression
