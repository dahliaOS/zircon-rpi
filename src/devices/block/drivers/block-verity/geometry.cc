// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "geometry.h"

#include <zircon/assert.h>

#include <cstdio>
#include <utility>

namespace block_verity {

IntegrityShape IntegrityShapeFor(uint32_t block_size, uint32_t hash_size,
                                 uint64_t data_block_count) {
  ZX_ASSERT(block_size % hash_size == 0);
  uint32_t hashes_per_block = block_size / hash_size;
  uint64_t direct_hash_blocks_needed_floored = data_block_count / hashes_per_block;
  uint32_t remainder = data_block_count % hashes_per_block;
  uint64_t direct_hash_blocks_needed = direct_hash_blocks_needed_floored + (remainder != 0 ? 1 : 0);
  if (direct_hash_blocks_needed == 1) {
    // Base case - fits in a block, which goes at the root
    return IntegrityShape{1,1};
  }

  IntegrityShape indirect = IntegrityShapeFor(block_size, hash_size, direct_hash_blocks_needed);
  uint64_t total_blocks_needed = direct_hash_blocks_needed + indirect.integrity_block_count;
  uint32_t tree_depth = 1 + indirect.tree_depth;
  return IntegrityShape { total_blocks_needed, tree_depth };
}

BlockAllocation BestSplitFor(uint32_t block_size, uint32_t hash_size, uint64_t total_blocks) {
  // Block_size must be a multiple of hash_size, because we don't want to deal
  // with padding and both are almost always powers of two anyway.
  ZX_ASSERT(block_size % hash_size == 0);
  // must have at least three blocks to split - one superblock, one data block, one integrity block.
  ZX_ASSERT(total_blocks >= 3);

  uint64_t superblocks = 1;
  uint64_t largest_possible_data_blocks = 1;
  uint64_t smallest_impossible_data_blocks = total_blocks - 2;
  IntegrityShape best_integrity_shape_yet;

  while (largest_possible_data_blocks + 1 < smallest_impossible_data_blocks) {
    // Binary search to find most data blocks we can support.
    uint64_t test_data_blocks = largest_possible_data_blocks + ((smallest_impossible_data_blocks - largest_possible_data_blocks) / 2);
    IntegrityShape i = IntegrityShapeFor(block_size, hash_size, test_data_blocks);
    if (test_data_blocks + i.integrity_block_count + superblocks <= total_blocks) {
      // Having test_data_blocks is satisfiable.
      largest_possible_data_blocks = test_data_blocks;
      best_integrity_shape_yet = i;
    } else {
      smallest_impossible_data_blocks = test_data_blocks;
    }
  }

  // It's possible at the margins that we can't make use of the entirety of the
  // block device -- if we were to add a data block, we'd need an additional
  // integrity block, because we're at the edge of an integrity block boundary
  // too, but we have none left to allocate.  In this case we allocate the
  // additional block (or blocks) to the end of the integrity section, where it
  // will sit unused.  That is: those blocks contribute to
  // padded_integrity_block_count in `BlockAllocation` here, but not to
  // IntegrityShape's `integrity_block_count`.
  uint64_t padded_integrity_blocks = total_blocks - superblocks - largest_possible_data_blocks;
  return BlockAllocation {
    superblocks,
    padded_integrity_blocks,
    largest_possible_data_blocks,
    best_integrity_shape_yet
  };
}

Geometry::Geometry(uint32_t block_size, uint32_t hash_size, uint64_t total_blocks) :
  //block_size_(block_size),
  //hash_size_(hash_size),
  //total_blocks_(total_blocks),
  hashes_per_block_(block_size / hash_size),
  allocation_(BestSplitFor(block_size, hash_size, total_blocks)) {
}

HashLocation Geometry::IntegrityDataLocationForDataBlock(DataBlockIndex data_block_index) {
  uint64_t to_pass = data_block_index / hashes_per_block_;

  uint64_t block_offset = 0;
  uint32_t tier = 0;

  while (to_pass > 0) {
    // Skipping |to_pass| blocks for tier |tier|
    block_offset += to_pass;
    to_pass = to_pass / hashes_per_block_;
    tier++;
  }

  uint32_t hash_offset = data_block_index % hashes_per_block_;
  return HashLocation{ block_offset, hash_offset };
}

HashLocation Geometry::NextIntegrityBlockUp(uint32_t distance_from_leaf,
                                            IntegrityBlockIndex integrity_block_index) {
  //
  // If, for example hashes_per_block_ were 128, the integrity data would look
  // like this, where reading blocks left to right (and the contents of the
  // boxes) indicates the block offset within the integrity section, and each
  // block in tier N+1 contains the hashes of the |hashes_per_block_| preceding
  // blocks from tier N (and blocks in tier 0 contain hashes of blocks from the
  // data section.
  //
  // tier 2                                                                      |16512|         ...
  // tier 1                        |128|                       |257| ... |16511|                 ...
  // tier 0  |0| |1| |2| ... |127|       |129| |130| ... |256|       ...                 |16513| ...
  //
  // So, in this hypothetical example, if you passed distance_from_leaf = 0 and
  // integrity_block_index 2, you'd expect to get back a hash location with
  // integrity_block 128 and hash_in_block 2.
  //
  // The integrity block number for a given index is the first block to the
  // right of it in the next tier up.  So 2 -> 128, and 128 -> 16512.
  //
  // Note: |distance_from_leaf| is inferrable from integrity_block_number, and
  // what its value in base (hashes_per_block_+1) is, but that involves test
  // division which is slow.  Better to track the distance and save some integer
  // division/modular arithmetic.
  //
  // If integrity_block_index is a leaf node, distance_from_leaf should be 0.

  printf("                                          NextIntegrityBlockUp(block: %lu, tier %u)\n", integrity_block_index, distance_from_leaf);

  uint64_t one_indexed_integrity_block_index = integrity_block_index + 1;
  // Convert to one-indexed arithmetic for the next bit.  It's simpler for some
  // of the modular arithmetic around tier sizes

  uint64_t current_tier_size = 1;
  for (uint32_t i = 0; i < distance_from_leaf; i++) {
    current_tier_size *= hashes_per_block_;
    current_tier_size += 1;
  }
  uint64_t next_tier_size = current_tier_size * hashes_per_block_ + 1;

  printf("                                          current tier size %lu, next tier size %lu\n", current_tier_size, next_tier_size);

  // Compute which hash in this integrity block covers `integrity_block_index`.
  // We can achieve this by operating modulo the next larger tier size, and dividing
  // by the current tier size (which we know from above our current index is an
  // exact multiple of, unless it is in the last block of this tier).
  uint64_t block_in_tier_chunk = one_indexed_integrity_block_index % next_tier_size;
  uint32_t unadjusted_offset_within_block = block_in_tier_chunk / current_tier_size;

  // `block_in_tier_chunk` should be a perfect multiple of `current_tier_size`,
  // unless it is in the last block of this tier, which would cause the
  // value of `unadjusted_offset_within_block` to truncate when dividing.
  // We'd like to round that truncated bit up.
  //
  // In the former case, we need to subtract one to return to zero-indexing.
  // In the latter case, we need to subtract one to return to zero-indexing and
  // we need to add one to compensate for the truncating division, which means
  // we can just take the value as-is.
  uint32_t offset_within_block = (unadjusted_offset_within_block * current_tier_size == block_in_tier_chunk) ?
      unadjusted_offset_within_block - 1 :
      unadjusted_offset_within_block;

  // Round up to the next multiple of the next tier size, by shaving off the
  // residue mod next_tier_size, then adding in the full next_tier_size.
  // There's probably another way to compute this in closed form that's faster
  // and uses smaller numbers by reusing offset_within_block.
  uint64_t one_indexed_containing_block_index = (one_indexed_integrity_block_index - (one_indexed_integrity_block_index % next_tier_size) + next_tier_size);
  //uint64_t one_indexed_containing_block_index = (one_indexed_integrity_block_index - (offset_within_block * current_tier_size) - 1 + next_tier_size);

  uint64_t zero_indexed_containing_block_index = one_indexed_containing_block_index - 1;

  // Clamp block index to the total number of populated integrity blocks -- the
  // very last block at each tier may appear earlier than it would in a full
  // tree.  This case is easily detected -- we can compute the "last possible
  // block" for this tier, and just clamp to that block if we would otherwise
  // have exceeded it.
  uint64_t max_block_index_at_tier = allocation_.integrity_shape.integrity_block_count - (allocation_.integrity_shape.tree_depth - 1 - distance_from_leaf);

  uint64_t clamped_containing_block_index = zero_indexed_containing_block_index;
  if (zero_indexed_containing_block_index > max_block_index_at_tier) {
    // Don't reach too far
    clamped_containing_block_index = max_block_index_at_tier;
    //printf("clampsed %u to %u\n", zero_indexed_containing_block_index, max_block_index_at_tier);
  }
  return HashLocation{ clamped_containing_block_index, offset_within_block };
}

}  // namespace block_verity
