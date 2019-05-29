// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddktl/device.h>

namespace aml_usb_phy {

// Stub device for binding USB controller drivers.
class ChildDevice;
using ChildDeviceType = ddk::Device<ChildDevice>;

// This is the main class for the platform bus driver.
class ChildDevice : public ChildDeviceType {
public:
    explicit ChildDevice(zx_device_t* parent)
        : ChildDeviceType(parent) {}

    // Device protocol implementation.
    void DdkRelease() {
        delete this;
    }

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(ChildDevice);
};

} // namespace aml_usb_phy
