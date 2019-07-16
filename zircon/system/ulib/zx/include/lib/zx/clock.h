// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ZX_CLOCK_H_
#define LIB_ZX_CLOCK_H_

#include <lib/zx/handle.h>
#include <lib/zx/object.h>
#include <lib/zx/time.h>
#include <zircon/syscalls/clock.h>

namespace zx {

class clock final : public object<clock> {
public:
    struct update_args : public ::zx_clock_update_args_t {
        constexpr update_args() = default;

        update_args& reset() {
            flags = 0;
            return *this;
        }

        update_args& set_value(int64_t value) {
            this->value = value;
            this->flags |= ZX_CLOCK_UPDATE_FLAG_VALUE_VALID;
            return *this;
        }

        update_args& set_rate_adjust(int32_t rate) {
            this->rate_adjust = rate;
            this->flags |= ZX_CLOCK_UPDATE_FLAG_RATE_ADJUST_VALID;
            return *this;
        }

        update_args& set_error_bound(uint64_t error_bound) {
            this->error_bound = error_bound;
            this->flags |= ZX_CLOCK_UPDATE_FLAG_ERROR_BOUND_VALID;
            return *this;
        }
    };

    static constexpr zx_obj_type_t TYPE = ZX_OBJ_TYPE_CLOCK;

    constexpr clock() = default;

    explicit clock(zx_handle_t value) : object(value) {}

    explicit clock(handle&& h) : object(h.release()) {}

    clock(clock&& other) : object(other.release()) {}

    clock& operator=(clock&& other) {
        reset(other.release());
        return *this;
    }

    static zx_status_t create(uint32_t options, clock* result) {
        return zx_clock_create(options, result->reset_and_get_address());
    }

    zx_status_t read(zx_time_t* now_out) {
        return zx_clock_read(value_, now_out);
    }

    zx_status_t get_details(zx_clock_details_t* details_out) {
        return zx_clock_get_details(value_, details_out);
    }

    zx_status_t update(const update_args& args) {
        return zx_clock_update(value_, static_cast<const zx_clock_update_args_t*>(&args));
    }

    template <zx_clock_t kClockId>
    static zx_status_t get(basic_time<kClockId>* result) {
        return zx_clock_get(kClockId, result->get_address());
    }

    static time get_monotonic() {
        return time(zx_clock_get_monotonic());
    }
};

using unowned_clock = unowned<clock>;

} // namespace zx

#endif  // LIB_ZX_CLOCK_H_
