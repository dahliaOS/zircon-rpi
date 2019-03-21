// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/amlogiccanvas.h>
#include <ddk/protocol/clock.h>
#include <ddk/protocol/gpio.h>
#include <ddk/protocol/i2c.h>
#include <ddk/protocol/platform/device.h>
#include <ddk/protocol/power.h>
#include <ddk/protocol/sysmem.h>
#include <ddktl/device.h>
#include <lib/zx/channel.h>

namespace component {

class Component;
using ComponentBase = ddk::Device<Component, ddk::Rxrpcable, ddk::Unbindable>;

class Component : public ComponentBase {
public:
    explicit Component(zx_device_t* parent);

    static zx_status_t Bind(void* ctx, zx_device_t* parent);

    zx_status_t DdkRxrpc(zx_handle_t channel);
    void DdkUnbind();
    void DdkRelease();

private:
    amlogic_canvas_protocol_t canvas_ = {};
    clock_protocol_t clock_ = {};
    gpio_protocol_t gpio_ = {};
    i2c_protocol_t i2c_ = {};
    pdev_protocol_t pdev_ = {};
    power_protocol_t power_ = {};
    sysmem_protocol_t sysmem_ = {};
};

} // namespace component
