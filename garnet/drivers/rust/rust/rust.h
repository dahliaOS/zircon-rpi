// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RUST_DRIVER_EXAMPLE_H_
#define RUST_DRIVER_EXAMPLE_H_

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <stdlib.h>
#include <zircon/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns an integer from the Rust crate.
zx_status_t rust_example_bind(void* ctx, zx_device_t* device);

#ifdef __cplusplus
}
#endif

#endif  // RUST_DRIVER_EXAMPLE_H_

