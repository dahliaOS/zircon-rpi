// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SYSROOT_ZIRCON_SYSCALLS_CLOCK_H_
#define SYSROOT_ZIRCON_SYSCALLS_CLOCK_H_

#include <zircon/time.h>

// clang-format off

// Clock creation options.
#define ZX_CLOCK_OPT_MONOTONIC                  ((uint32_t)1u << 0)
#define ZX_CLOCK_OPT_CONTINUOUS                 ((uint32_t)1u << 1)

#define ZX_CLOCK_OPTS_ALL ( \
        ZX_CLOCK_OPT_MONOTONIC | \
        ZX_CLOCK_OPT_CONTINUOUS)

// Clock update flags
#define ZX_CLOCK_UPDATE_FLAG_VALUE_VALID        ((uint32_t)1u << 0)
#define ZX_CLOCK_UPDATE_FLAG_RATE_ADJUST_VALID  ((uint32_t)1u << 1)
#define ZX_CLOCK_UPDATE_FLAG_ERROR_BOUND_VALID  ((uint32_t)1u << 2)

#define ZX_CLOCK_UPDATE_FLAGS_ALL ( \
        ZX_CLOCK_UPDATE_FLAG_VALUE_VALID |  \
        ZX_CLOCK_UPDATE_FLAG_RATE_ADJUST_VALID | \
        ZX_CLOCK_UPDATE_FLAG_ERROR_BOUND_VALID)

// Clock rate adjustment limits
#define ZX_CLOCK_UPDATE_MIN_RATE_ADJUST         ((int32_t)-1000)
#define ZX_CLOCK_UPDATE_MAX_RATE_ADJUST         ((int32_t)1000)

// Special clock error values
#define ZX_CLOCK_UNKNOWN_ERROR                  ((uint64_t)0xFFFFFFFFFFFFFFFF)

// clang-format on

typedef struct zx_clock_rate {
    uint32_t synthetic_ticks;
    uint32_t reference_ticks;
} zx_clock_rate_t;

typedef struct zx_clock_transformation {
    int64_t reference_offset;
    int64_t synthetic_offset;
    zx_clock_rate_t rate;
} zx_clock_transformation_t;

typedef struct zx_clock_details {
    uint32_t generation_counter;
    uint32_t options;
    zx_clock_transformation_t ticks_to_synthetic;
    zx_clock_transformation_t mono_to_synthetic;
    uint64_t error_bound;
    zx_ticks_t query_ticks;
    zx_ticks_t last_value_update_ticks;
    zx_ticks_t last_rate_adjust_update_ticks;
    zx_ticks_t last_error_bounds_update_ticks;
} zx_clock_details_t;

typedef struct zx_clock_update_args {
#ifdef __cplusplus
    constexpr zx_clock_update_args()
        : flags(0),
          rate_adjust(0),
          value(0),
          error_bound(0) {}
#endif
    uint32_t flags;
    int32_t  rate_adjust;
    int64_t  value;
    uint64_t error_bound;
} zx_clock_update_args_t;

#endif  // SYSROOT_ZIRCON_SYSCALLS_CLOCK_H_
