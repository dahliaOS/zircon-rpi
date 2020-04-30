// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "geometry.h"

#include <zxtest/zxtest.h>

namespace {

TEST(GeometryTest, IntegrityShapeFor4kSHA256) {
  block_verity::IntegrityShape i = block_verity::IntegrityShapeFor(4096, 32, 8192);
  // The optimal shape is 1 superblock, 65 integrity blocks, and 8126 data blocks.
  // 8126 / 128 = 63.48 or so.
  // We need 64 direct hash blocks, and one indirect hash block, which contains
  // hashes of the direct hash blocks (which themselves contain hashes of the
  // data blocks).
  ASSERT_EQ(i.integrity_block_count, 65);
  ASSERT_EQ(i.tree_depth, 2);
}

TEST(GeometryTest, IntegrityShapeForAssertsIfHashNotMultipleOfBlockSize) {
  ASSERT_DEATH([] {
    block_verity::IntegrityShapeFor(4096, 33, 8192);
  }, "IntegrityShapeFor should assert if block_size modulo hash_size is not 0");
}

TEST(GeometryTest, BestSplitFor) {
  block_verity::BlockAllocation a = block_verity::BestSplitFor(4096, 32, 3);
  ASSERT_EQ(a.superblock_count, 1);
  ASSERT_EQ(a.padded_integrity_block_count, 1);
  ASSERT_EQ(a.data_block_count, 1);
  ASSERT_EQ(a.superblock_count + a.padded_integrity_block_count + a.data_block_count, 3);

  // Verify that we smoothly allocate additional blocks, and that we always
  // allocate all blocks, from the smallest possible partition (3 blocks) up to
  // ~32MiB on 4k blocks with SHA256 hash function.
  block_verity::BlockAllocation prev = a;
  for (uint64_t block_count = 4; block_count <= 8192; block_count++) {
    block_verity::BlockAllocation ba = block_verity::BestSplitFor(4096, 32, block_count);
    ASSERT_EQ(ba.superblock_count + ba.padded_integrity_block_count + ba.data_block_count, block_count);
    ASSERT_EQ(ba.superblock_count, 1);

    bool changed_integrity = (ba.padded_integrity_block_count != prev.padded_integrity_block_count);
    bool changed_data = (ba.data_block_count != prev.data_block_count);
    ASSERT_TRUE(changed_integrity != changed_data);
    if (changed_integrity) {
      ASSERT_EQ(ba.padded_integrity_block_count, prev.padded_integrity_block_count + 1);
    }
    if (changed_data) {
      ASSERT_EQ(ba.data_block_count, prev.data_block_count + 1);
    }
    prev = ba;
  }
}

TEST(GeometryTest, BestSplitForAssertsIfTooSmall) {
  ASSERT_DEATH([] {
    block_verity::BestSplitFor(4096, 32, 2);
  }, "BestSplitFor should assert if total_blocks is less than 3");
}



}  // namespace


/*
int main(void) {
  block_verity::IntegrityShape i = block_verity::IntegrityShapeFor(4096, 32, 300);
  ASSE

  printf("integrity_block_count: %u\ntree_depth: %u\n", i.integrity_block_count, i.tree_depth);
  //for (uint32_t block_count = 3; block_count < 8193; block_count++) {
  //  BlockAllocation g = BestSplitFor(4096, 32, block_count);
  //  printf("BestSplitFor(4096, 32, %u): "
  //         "superblocks: %u "
  //         "integrity: %u "
  //         "data: %u\n",
  //         block_count,
  //         g.superblock_count, g.padded_integrity_block_count, g.data_block_count);
  //}
  // note: three tiers of hash tree is sufficient up to like 16GiB on 4k blocks
  //uint64_t test_size = 524288; // 2 GiB partition
  uint64_t test_size = 2130050; // enough to get 4 hash tiers with sha256/4k blocks
  //uint64_t test_size = 65536;
  block_verity::Geometry g(4096, 32, test_size);
  block_verity::BlockAllocation ba = block_verity::BestSplitFor(4096, 32, test_size);
  printf("BestSplitFor(4096, 32, %lu): "
         "superblocks: %lu "
         "integrity: %lu "
         "data: %lu\n",
         test_size,
         ba.superblock_count, ba.padded_integrity_block_count, ba.data_block_count);
  //for (uint64_t i = 507901; i < g.allocation_.data_block_count; i++) {
  for (uint64_t i = 0; i < g.allocation_.data_block_count; i++) {
    uint64_t data_block_absolute_position = i + g.allocation_.superblock_count + g.allocation_.padded_integrity_block_count;
    printf("data block %lu: block %lu\n", i, data_block_absolute_position);

    block_verity::HashLocation h = g.IntegrityDataLocationForDataBlock(i);
    printf(" leaf hash at block %lu offset %u\n", h.integrity_block, h.hash_in_block);
    uint64_t next_block = h.integrity_block;
    int distance_from_leaf = 0;
    while (next_block != g.integrity_shape_.integrity_block_count - 1) {
      block_verity::HashLocation ph = g.NextIntegrityBlockUp(distance_from_leaf, next_block);
      for (int x = -2 ; x < distance_from_leaf ; x++) {
        printf(" ");
      }
      printf("hash at block %lu offset %u\n", ph.integrity_block, ph.hash_in_block);
      next_block = ph.integrity_block;
      distance_from_leaf++;
    }
  }

  return 0;
}
*/
