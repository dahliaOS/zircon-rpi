// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_CLK_CLOCKTREE_INCLUDE_DEV_CLK_CLOCKTREE_CLOCKTREE_H_
#define ZIRCON_SYSTEM_DEV_CLK_CLOCKTREE_INCLUDE_DEV_CLK_CLOCKTREE_CLOCKTREE_H_

#include <stdint.h>
#include <dev/clk/clocktree/baseclock.h>
#include <zircon/types.h>

namespace clk {

class ClockTree {
  public:
    explicit ClockTree(BaseClock** clocks, const uint32_t count)
        : clocks_(clocks), count_(count) {}
    
    zx_status_t Enable(const uint32_t id);
    zx_status_t Disable(const uint32_t id);
    zx_status_t IsEnabled(const uint32_t id, bool* out);

    zx_status_t SetRate(const uint32_t id, const Hertz rate);
    zx_status_t QuerySupportedRate(const uint32_t id, const Hertz max);
    zx_status_t GetRate(const uint32_t id, Hertz* out);

    zx_status_t SetInput(const uint32_t id, const uint32_t input_index);
    zx_status_t GetNumInputs(const uint32_t id, uint32_t* out);
    zx_status_t GetInput(const uint32_t id, uint32_t* out);
  private:
    bool InRange(const uint32_t index) const;

    BaseClock** clocks_;
    uint32_t count_;
};

}  // namespace clk

#endif  // ZIRCON_SYSTEM_DEV_CLK_CLOCKTREE_INCLUDE_DEV_CLK_CLOCKTREE_CLOCKTREE_H_
