// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/protocol/i2c.h>
#include <ddktl/device.h>
#include <ddktl/i2c-channel.h>
#include <ddktl/protocol/codec.h>

namespace audio {

class Tas5805;
using DeviceType = ddk::Device<Tas5805, ddk::Unbindable>;

class Tas5805 final : public DeviceType,
                      public ddk::CodecProtocol<Tas5805, ddk::base_protocol> {
public:
    static std::unique_ptr<Tas5805> Create(ddk::I2cChannel i2c, uint32_t i2c_index);
    static zx_status_t Create(zx_device_t* parent);

    explicit Tas5805(zx_device_t* device, const ddk::I2cChannel& i2c)
        : DeviceType(device), i2c_(i2c) {}
    zx_status_t Bind();

    void DdkRelease() {
        delete this;
    }
    void DdkUnbind() {
        DdkRemove();
    }

    void CodecInitialize(codec_initialize_callback callback, void* cookie);
    void CodecGetGainFormat(codec_get_gain_format_callback callback, void* cookie);
    void CodecGetGain(codec_get_gain_callback callback, void* cookie);
    void CodecSetGain(float gain, codec_set_gain_callback callback, void* cookie);
    void CodecGetDaiFormats(codec_get_dai_formats_callback callback, void* cookie);
    void CodecSetDaiFormat(const dai_format_t* format,
                           codec_set_dai_format_callback callback, void* cookie);

    bool ValidGain(float gain) const;
    zx_status_t Reset();
    zx_status_t Standby() ;
    zx_status_t ExitStandby() ;


private:
    static constexpr float kMaxGain = 24.0;
    static constexpr float kMinGain = -103.0;
    static constexpr float kGainStep = 0.5;

    zx_status_t WriteReg(uint8_t reg, uint8_t value);

    zx_status_t SetStandby(bool stdby);

    ddk::I2cChannel i2c_;

    float current_gain_ = 0;
};
} // namespace audio
