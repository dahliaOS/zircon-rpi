// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zbitl/checking.h>

namespace zbitl {

template <>
std::string_view CheckHeader<Checking::kPermissive>(const zbi_header_t& header, uint32_t capacity) {
  // TODO: checks
  return {};
}

template <>
std::string_view CheckHeader<Checking::kStrict>(const zbi_header_t& header, uint32_t capacity) {
  if (auto check = CheckHeader<Checking::kPermissive>(header, capacity); !check.empty()) {
    return check;
  }
  // TODO: more checks
  return {};
}

}  // namespace zbitl
