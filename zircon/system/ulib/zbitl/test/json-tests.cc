// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <lib/zbitl/fd.h>
#include <lib/zbitl/json.h>

#include <rapidjson/filewritestream.h>
#include <rapidjson/prettywriter.h>

#include "file-tests.h"

namespace {

constexpr size_t kJsonBufferSize = BUFSIZ;

using namespace zbitl::test;

TEST(ZbitlJsonTests, Basic) {
  auto filename = TestFileName(kTestFile);
  fbl::unique_fd fd(open(filename.c_str(), O_RDONLY));
  ASSERT_TRUE(fd, "cannot open '%s': %s", filename.c_str(), strerror(errno));

  zbitl::View<const fbl::unique_fd&> view(fd);
  EXPECT_TRUE(fd, "non-movable zbitl::View argument moved/reset fd?");

  // TODO(49438): Basic test should write the JSON in memory from a constexpr
  // ZBI blob and compare to a constexpr golden JSON string.  Fancier tests can
  // read/and or write files.

  char buffer[kJsonBufferSize];
  rapidjson::FileWriteStream json_stream(stdout, buffer, sizeof(buffer));
  rapidjson::PrettyWriter<rapidjson::FileWriteStream> json_writer(json_stream);
  json_writer.SetIndent(' ', 2);
  JsonWriteZbi(json_writer, view, 0);

  auto error = view.take_error();
  EXPECT_FALSE(error, "%s errno=%d (%s)",
               std::string(error->zbi_error).c_str(),  // Technically no '\0'.
               error->storage_error ? *error->storage_error : 0,
               error->storage_error ? strerror(*error->storage_error) : "no storage_error");
}

}  // namespace
