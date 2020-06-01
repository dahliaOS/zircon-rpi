// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_ZBITL_TEST_FILE_TESTS_H_
#define ZIRCON_SYSTEM_ULIB_ZBITL_TEST_FILE_TESTS_H_

#include <lib/zbitl/view.h>

#include <cstdlib>
#include <string>
#include <string_view>

#include <zxtest/zxtest.h>

namespace zbitl {
namespace test {

constexpr std::string_view kTestFile = "zbitl-test.zbi";

inline std::string TestFileName(std::string_view name) {
  static auto test_root_dir = []() -> std::string_view {
    const char* env = getenv("TEST_ROOT_DIR");
    if (env) {
      return env;
    }
    return {};
  }();
  std::string file(test_root_dir);
  if (!file.empty()) {
    file += "/";
  }
  file += name;
  return file;
}

}  // namespace test
}  // namespace zbitl

#endif  // ZIRCON_SYSTEM_ULIB_ZBITL_TEST_FILE_TESTS_H_
