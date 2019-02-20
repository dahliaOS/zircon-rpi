// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>

#include "stream.h"

namespace ioqueue {

Stream::Stream(uint32_t pri) : priority_(pri), flags_(0) {
    list_initialize(&ready_op_list_);
    list_initialize(&issued_op_list_);
}

Stream::~Stream() {
    assert(list_is_empty(&ready_op_list_));
    assert(list_is_empty(&issued_op_list_));
}

} // namespace
