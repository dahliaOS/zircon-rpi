// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dev/clk/clocktree/baseclock.h>

namespace clk {

zx_status_t BaseClock::EnableInternal() {
    const uint32_t enable_count = EnableCount();

    zx_status_t result = ZX_OK;
    if (enable_count == 0) {
        // This is the first vote on this clock, let's try to enable it.
        result = Enable();
    }

    // There are 3 cases where we increment the enable count:
    // (1) The enable count was already > 0 so we took no action and incremented the vote
    // (2) The enable count was previously 0 and we successfully enabled the clock.
    // (3) The enable count was 0 but this clock doesn't support gating/ungating.
    if (result == ZX_OK || result == ZX_ERR_NOT_SUPPORTED) {
        EnableCount(enable_count + 1);
    }

    return result;
}

zx_status_t BaseClock::DisableInternal() {
    const uint32_t enable_count = EnableCount();
    const uint32_t final_enable_count = enable_count == 0 ? 0 : enable_count - 1;

    zx_status_t result = ZX_OK;
    if (enable_count == 1) {
        result = Disable();
    }

    if (result == ZX_OK || result == ZX_ERR_NOT_SUPPORTED) {
        EnableCount(final_enable_count);
    }

    return result;
}


}  // namespace clk
