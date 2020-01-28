// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "blob-verifier.h"

#include <zircon/status.h>

#include <digest/digest.h>
#include <digest/merkle-tree.h>
#include <fs/trace.h>

namespace blobfs {

BlobVerifier::BlobVerifier(digest::Digest digest, digest::MerkleTreeVerifier mtv)
    : digest_(std::move(digest)), tree_verifier_(std::move(mtv)) {}

zx_status_t BlobVerifier::Create(digest::Digest digest, void* merkle, size_t merkle_size,
                                 size_t data_size, BlobVerifier* out) {
  digest::MerkleTreeVerifier mtv;
  zx_status_t status = mtv.SetDataLength(data_size);
  if (status != ZX_OK || mtv.GetTreeLength() > merkle_size) {
    FS_TRACE_ERROR("Failed to load merkle tree, invalid size\n");
    return status;
  }
  if ((status = mtv.SetTree(merkle, merkle_size, digest.get(), digest.len())) != ZX_OK) {
    FS_TRACE_ERROR("Failed to load merkle tree: %s\n", zx_status_get_string(status));
  }

  *out = BlobVerifier(std::move(digest), std::move(mtv));
  return ZX_OK;
}

zx_status_t BlobVerifier::Verify(void* data, size_t data_size) {
  TRACE_DURATION("blobfs", "BlobVerifier::Verify");
  zx_status_t status = tree_verifier_.Verify(data, data_size, 0);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Verify(%s) failed: %s\n", digest_.ToString().c_str(),
                   zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

zx_status_t BlobVerifier::VerifyPartial(void* data, size_t offset, size_t length) {
  TRACE_DURATION("blobfs", "BlobVerifier::VerifyPartial");
  zx_status_t status = tree_verifier_.Verify(data, length, offset);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Verify(%s) failed: %s\n", digest_.ToString().c_str(),
                   zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

}  // namespace blobfs
