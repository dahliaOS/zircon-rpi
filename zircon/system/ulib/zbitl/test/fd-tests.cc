// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <lib/zbitl/fd.h>

#include <cinttypes>

#include "file-tests.h"

namespace {

using namespace zbitl::test;

TEST(ZbitlFdTests, Basic) {
  auto filename = TestFileName(kTestFile);
  fbl::unique_fd fd(open(filename.c_str(), O_RDONLY));
  ASSERT_TRUE(fd, "cannot open '%s': %s", filename.c_str(), strerror(errno));

  zbitl::View<const fbl::unique_fd&> view(fd);
  EXPECT_TRUE(fd, "non-movable zbitl::View argument moved/reset fd?");

  for (auto [header, payload] : view) {
    // Hidden lambdas and auto [...] and C++ and oy.
    const uint32_t flags = header->flags;
    const uint64_t offset = static_cast<uint64_t>(payload);
    EXPECT_TRUE(flags & ZBI_FLAG_VERSION, "flags %#x payload offset %#" PRIx64, flags, offset);
  }

  auto error = view.take_error();
  EXPECT_FALSE(error, "%s errno=%d (%s)",
               std::string(error->zbi_error).c_str(),  // Technically no '\0'.
               error->storage_error ? *error->storage_error : 0,
               error->storage_error ? strerror(*error->storage_error) : "no storage_error");
}

TEST(ZbitlFdTests, Move) {
  auto filename = TestFileName(kTestFile);
  fbl::unique_fd fd(open(filename.c_str(), O_RDONLY));
  ASSERT_TRUE(fd, "cannot open '%s': %s", filename.c_str(), strerror(errno));

  zbitl::View view(std::move(fd));
  EXPECT_FALSE(fd, "movable zbitl::View fd argument did not move/reset fd?");

  for (auto [header, payload] : view) {
    EXPECT_TRUE(header->flags & ZBI_FLAG_VERSION);
  }

  auto error = view.take_error();
  EXPECT_FALSE(error, "%s errno=%d (%s)",
               std::string(error->zbi_error).c_str(),  // Technically no '\0'.
               error->storage_error ? *error->storage_error : 0,
               error->storage_error ? strerror(*error->storage_error) : "no storage_error");
}

TEST(ZbitlFdTests, Error) {
  fbl::unique_fd nofd;
  ASSERT_FALSE(nofd);

  zbitl::View view(std::move(nofd));
  for (auto [header, payload] : view) {
    EXPECT_EQ(header->flags, header->flags);
    EXPECT_TRUE(false, "should not be reached");
  }

  auto error = view.take_error();
  ASSERT_TRUE(error, "no error when header cannot be read??");
  EXPECT_FALSE(error->zbi_error.empty(), "empty zbi_error string!!");
  EXPECT_TRUE(error->storage_error.has_value(), "read error not propagated?");
  EXPECT_EQ(*error->storage_error, EBADF, "expected %s (EBADF), not %s", strerror(EBADF),
            strerror(*error->storage_error));
}

}  // namespace
