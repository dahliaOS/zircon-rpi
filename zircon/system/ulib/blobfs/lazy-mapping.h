// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_BLOBFS_LAZY_MAPPING_H_
#define ZIRCON_SYSTEM_ULIB_BLOBFS_LAZY_MAPPING_H_

#include <lib/zx/vmo.h>

#include <fbl/function.h>
#include <fxl/macros.h>
#include <fzl/owned-vmo-mapper.h>
#include <zircon/status.h>

namespace blobfs {

// LazyMapping is a VMO-backed mapping that is lazily instantiated on first access.
//
// On the first call to GetVmo(), all of the pages for the VMO (both merkle metadata and data) are
// mapped in.
class LazyMapping {
 public:
  LazyMapping(const Inode& inode, BlobLoader* loader);

  // Fetches the VMO underlying the mapping, mapping in the VMO on first access.
  zx_status_t GetVmo(zx::unowned_vmo* out);

  // Unmap the VMO and release any underlying memory.
  void Reset();

 protected:
  zx::vmo vmo_;
  bool is_mapped_ = false;
  BlobLoader* loader_;
  Inode inode_;

 private:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LazyMapping);
};

// PagedLazyMapping is a variant of LazyMapping that defers loading data pages until a client
// accesses those pages.
//
// On the first call to GetVmo(), all of the merkle metadata for the VMO is mapped in, and the
// data pages are set up to be mapped in via a pager when clients access the page.
class PagedLazyMapping : public LazyMapping {
 public:
  PagedLazyMapping(const Inode& inode, BlobLoader* loader);

  // Fetches the VMO underlying the mapping. Metadata will be mapped in immediately, but data
  // is mapped in lazily as clients access the pages.
  zx_status_t GetVmo(zx::unowned_vmo* out) override;

 private:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PagedLazyMapping);
};

}  // namespace blobfs

#endif  // ZIRCON_SYSTEM_ULIB_BLOBFS_LAZY_MAPPING_H_
