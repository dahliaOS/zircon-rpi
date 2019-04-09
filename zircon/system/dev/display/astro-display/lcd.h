// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <unistd.h>
#include <zircon/compiler.h>
#include <ddk/protocol/platform/device.h>
#include <fbl/unique_ptr.h>
#include <hwreg/mmio.h>
#include <ddktl/protocol/gpio.h>
#include <ddktl/protocol/dsiimpl.h>

namespace astro_display {

class Lcd {
public:
    Lcd(uint8_t panel_type, zx_device_t* gpio, const ddk::DsiImplProtocolClient& dsi)
        : panel_type_(panel_type), gpio_(gpio), dsiimpl_(dsi) {}

    zx_status_t Init();
    zx_status_t Enable();
    zx_status_t Disable();
private:
    zx_status_t LoadInitTable(const uint8_t* buffer, size_t size);
    zx_status_t GetDisplayId();

    uint8_t                                     panel_type_;
    const ddk::GpioProtocolClient               gpio_;
    const ddk::DsiImplProtocolClient            dsiimpl_;

    bool                                        initialized_ = false;
    bool                                        enabled_ =false;
};

} // namespace astro_display
