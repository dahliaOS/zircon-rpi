// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/weave/weavestack/app.h"

static constexpr int RET_OK = 0;
static constexpr int RET_INIT_ERR = 1;
static constexpr int RET_RUN_ERR = 2;

int main(void) {
  weavestack::App app;

  if (app.Init() != WEAVE_NO_ERROR) {
    return RET_INIT_ERR;
  }

  if (app.Run() != ZX_OK) {
    return RET_RUN_ERR;
  }

  return RET_OK;
}
