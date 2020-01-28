// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_BLOBFS_BLOB_VERIFIER_H_
#define ZIRCON_SYSTEM_ULIB_BLOBFS_BLOB_VERIFIER_H_

#include <zircon/status.h>
#include <zircon/types.h>

#include <digest/digest.h>
#include <digest/merkle-tree.h>
#include <fbl/macros.h>

namespace blobfs {

// BlobVerifier verifies the contents of a blob against a merkle tree.
class BlobVerifier {
 public:
  BlobVerifier() = default;
  BlobVerifier(BlobVerifier&& other) = default;
  BlobVerifier& operator=(BlobVerifier&& other) = default;
  explicit BlobVerifier(digest::Digest digest, digest::MerkleTreeVerifier mtv);

  // Creates an instance of BlobVerifier for blobs named |digest|, using the provided merkle
  // tree.
  //
  // Returns an error if the merkle tree's root does not match |digest|, or if the tree's size
  // does not match the expected size for |data_size|.
  static zx_status_t Create(digest::Digest digest, void* merkle, size_t merkle_size,
                            size_t data_size, BlobVerifier* out);

  // Verifies the entire contents of a blob.
  zx_status_t Verify(void* data, size_t data_size);

  // Verifies a range of the contents of a blob from [offset, offset + length).
  zx_status_t VerifyPartial(void* data, size_t offset, size_t length);

 private:

  digest::Digest digest_;
  digest::MerkleTreeVerifier tree_verifier_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BlobVerifier);
};

}  // namespace blobfs

#endif  // ZIRCON_SYSTEM_ULIB_BLOBFS_BLOB_VERIFIER_H_
