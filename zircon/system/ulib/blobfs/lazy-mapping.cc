// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lazy-mapping.h"

#include <lib/zx/vmo.h>
#include <zircon/error.h>
#include <zircon/status.h>

namespace blobfs {

LazyMapping::LazyMapping(const Inode& inode, BlobLoader* loader) : loader_(loader), inode_(inode) {}

zx_status_t LazyMapping::GetVmo(zx::unowned_vmo* out) {
  if (!is_mapped_) {
    if (zx_status_t status = loader_->LoadBlob(inode_, &vmo_)) {
      return status;
    }
    is_mapped_ = true;
  }
  *out = vmo_.borrow();
  return ZX_OK;
}

void LazyMapping::Reset() {
  vmo_.reset();
  is_mapped_ = false;
}

PagedLazyMapping::PagedLazyMapping(const Inode& inode, BlobLoader* loader)
  : LazyMapping(inode, loader) {}

zx_status_t PagedLazyMapping::GetVmo(zx::unowned_vmo* out) {
  if (!is_mapped_) {
    if (zx_status_t status = loader_->LoadPagedBlob(inode_, &vmo_)) {
      return status;
    }
    is_mapped_ = true;
  }
  *out = vmo_.borrow();
  return ZX_OK;
}

}  // namespace blobfs
