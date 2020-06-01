// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zbitl/view.h>

#include <zxtest/zxtest.h>

namespace {

alignas(ZBI_ALIGNMENT) constexpr char kEmptyZbiHeader[] =
    "\x42\x4f\x4f\x54\x00\x00\x00\x00\xe6\xf7\x8c\x86\x00\x00\x01\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x29\x17\x78\xb5\xd6\xe8\x87\x4a";
constexpr std::string_view kEmptyZbi(kEmptyZbiHeader, sizeof(kEmptyZbiHeader));

TEST(ZbitlViewTests, Basic) {
  zbitl::View view(kEmptyZbi);  // Yay deduction guides!
  for (auto [header, payload] : view) {
    EXPECT_EQ(header->type, header->type);
    EXPECT_TRUE(false, "should not be reached");
  }

  auto error = view.take_error();
  ASSERT_FALSE(error, "%s at offset %#x", std::string(error->zbi_error).c_str(),
               error->item_offset);
}

TEST(ZbitlViewTests, Error) {
  zbitl::View<std::string_view> view;
  for (auto [header, payload] : view) {
    EXPECT_EQ(header->type, header->type);
    EXPECT_TRUE(false, "should not be reached");
  }

  auto error = view.take_error();
  ASSERT_TRUE(error, "no error when header cannot be read??");
  EXPECT_FALSE(error->zbi_error.empty(), "empty zbi_error string!!");
  EXPECT_FALSE(error->storage_error.has_value(), "read past capacity?");
}

TEST(ZbitlViewTests, StorageError) {
  zbitl::View<std::tuple<>> view;
  for (auto [header, payload] : view) {
    EXPECT_EQ(header->type, header->type);
    EXPECT_TRUE(false, "should not be reached");
  }

  auto error = view.take_error();
  ASSERT_TRUE(error, "no error when header cannot be read??");
  EXPECT_FALSE(error->zbi_error.empty(), "empty zbi_error string!!");
  EXPECT_TRUE(error->storage_error.has_value(), "read error not propagated?");
}

}  // namespace
