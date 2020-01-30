// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_CLK_CLOCKTREE_INCLUDE_DEV_CLK_CLOCKTREE_BASECLOCK_H_
#define ZIRCON_SYSTEM_DEV_CLK_CLOCKTREE_INCLUDE_DEV_CLK_CLOCKTREE_BASECLOCK_H_

#include <stdint.h>
#include <limits>
#include <zircon/types.h>

namespace clk {

using Hertz = uint64_t;
constexpr uint32_t kClkNoParent = std::numeric_limits<uint32_t>::max();

// System clocks should inherit from this class to implement their own clock
// instances.
class BaseClock {
  public:
    BaseClock(const char* name, const uint32_t id)
        : name_(name), id_(id), enable_count_(0) {}
    virtual ~BaseClock() {}
    virtual zx_status_t Enable() { return ZX_ERR_NOT_SUPPORTED; }
    virtual zx_status_t Disable() { return ZX_ERR_NOT_SUPPORTED; }
    virtual zx_status_t IsEnabled(bool* out) { return ZX_ERR_NOT_SUPPORTED; }

    virtual zx_status_t SetRate(const Hertz rate, const Hertz parent_rate) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    virtual zx_status_t QuerySupportedRate(const Hertz max, const Hertz parent_rate, Hertz* out) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    virtual zx_status_t GetRate(const Hertz parent_rate, Hertz* out) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    virtual zx_status_t SetInput(const uint32_t index) { return ZX_ERR_NOT_SUPPORTED; }
    virtual zx_status_t GetNumInputs(uint32_t* out) { return ZX_ERR_NOT_SUPPORTED; }
    virtual zx_status_t GetInput(uint32_t* out) { return ZX_ERR_NOT_SUPPORTED; }

    const char* Name() const { return name_; }
    uint32_t Id() const { return id_; }

    virtual uint32_t ParentId() = 0;

    uint32_t EnableCount() { return enable_count_; }
    void EnableCount(uint32_t enable_count) { enable_count_ = enable_count; }

    zx_status_t EnableInternal();
    zx_status_t DisableInternal();
    zx_status_t IsEnabledInternal(bool* out);

    const char* name_;
    const uint32_t id_;
    uint32_t enable_count_;

};

}  // namespace clk

#endif  // ZIRCON_SYSTEM_DEV_CLK_CLOCKTREE_INCLUDE_DEV_CLK_CLOCKTREE_BASECLOCK_H_
