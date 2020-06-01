// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ZBITL_CHECKING_H_
#define LIB_ZBITL_CHECKING_H_

#include <zircon/assert.h>
#include <zircon/boot/image.h>

#include <string_view>

namespace zbitl {

/// TODO: doc
enum class Checking {
  /// TODO: doc
  kStrict,

  /// TODO: doc
  kPermissive,

  /// TODO: doc
  kCrc,
};

/// Returns empty if the header checks out, otherwise an error string.
/// The capacity argument is the maximum space left in the container,
/// starting from the position of this header (not its payload).
template <Checking = Checking::kStrict>
std::string_view CheckHeader(const zbi_header_t&, uint32_t capacity);

template <>
std::string_view CheckHeader<Checking::kStrict>(const zbi_header_t&, uint32_t capacity);

template <>
std::string_view CheckHeader<Checking::kPermissive>(const zbi_header_t&, uint32_t capacity);

// CRC-checking mode doesn't apply to the header.
template <>
inline std::string_view CheckHeader<Checking::kCrc>(const zbi_header_t& header, uint32_t capacity) {
  return CheckHeader<Checking::kStrict>(header, capacity);
}

/// Returns empty if and only if the ZBI is complete (bootable), otherwise an
/// error string.  This takes any zbitl::View type or any type that acts like
/// it.  Note this does not check for errors from zbi.take_error() so if Zbi is
/// zbitl::View then the caller must use zbi.take_error() afterwards.  This
/// function always scans every item so all errors Zbi::iterator detects will
/// be found.  But this function's return value only indicates if the items
/// that were scanned before any errors were encountered added up to a complete
/// ZBI (regardless of whether there were additional items with errors).
template <typename Zbi, Checking Check = Checking::kStrict,
          uint32_t BootfsType = ZBI_TYPE_STORAGE_BOOTFS,
          uint32_t KernelType
#ifdef __aarch64__
          = ZBI_TYPE_KERNEL_ARM64
#elif defined(__x86_64__)
          = ZBI_TYPE_KERNEL_X64
#endif
          >
std::string_view CheckComplete(Zbi&& zbi) {
  enum {
    kNoItems,
    kKernelFirst,
    kKernelLater,
  } kernel = kNoItems;
  bool bootfs = false;
  for (auto [header, payload] : zbi) {
    switch (header->type) {
      case KernelType:
        kernel = kernel == kNoItems ? kKernelFirst : kKernelLater;
        break;
      case BootfsType:
        bootfs = true;
        break;
    }
  }
  if (auto error = zbi.take_error()) {
    return error->zbi_error;
  }
  switch (kernel) {
    case kNoItems:
      return "empty ZBI";
    case kKernelLater:
      return "kernel item out of order: must be first";
    case kKernelFirst:
      if (bootfs) {  // It's complete.
        return {};
      }
      return "missing BOOTFS";
  }
  ZX_ASSERT_MSG(false, "unreachable");
}

}  // namespace zbitl

#endif  // LIB_ZBITL_CHECKING_H_
