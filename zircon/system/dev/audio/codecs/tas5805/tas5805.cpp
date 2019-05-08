// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tas5805.h"

#include <algorithm>
#include <memory>
#include <string.h>

#include <ddk/binding.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <ddk/protocol/i2c-lib.h>
#include <ddk/protocol/i2c.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>

namespace {
// clang-format off
constexpr uint8_t kRegSelectPage  = 0x00;
constexpr uint8_t kRegReset       = 0x01;
constexpr uint8_t kRegDeviceCtrl1 = 0x02;
constexpr uint8_t kRegDeviceCtrl2 = 0x03;
constexpr uint8_t kRegSapCtrl1    = 0x33;
constexpr uint8_t kRegDigitalVol  = 0x4C;

constexpr uint8_t kRegResetBitCtrl             = 0x01;
constexpr uint8_t kRegDeviceCtrl1BitsPbtlMode  = 0x04;
constexpr uint8_t kRegDeviceCtrl1Bits1SpwMode  = 0x01;
constexpr uint8_t kRegSapCtrl1Bits32bits       = 0x03;
constexpr uint8_t kRegDeviceCtrl2BitsDeepSleep = 0x00;
constexpr uint8_t kRegDeviceCtrl2BitsPlay      = 0x03;
// clang-format on

// TODO(andresoportus): Add support for the other formats supported by this codec.
static const sample_format_t supported_sample_formats[] = { SAMPLE_FORMAT_FORMAT_I2S };
static const justify_format_t supported_justify_formats[] = { JUSTIFY_FORMAT_JUSTIFY_I2S };
static const uint32_t supported_rates[] = { 48000 };
static const uint8_t supported_bits_per_sample[] = { 32 };
static const dai_available_formats_t kSupportedDaiFormats = {
    .n_lanes = 1,
    .n_channels = 2,
    .sample_formats_list = supported_sample_formats,
    .sample_formats_count = countof(supported_sample_formats),
    .justify_formats_list = supported_justify_formats,
    .justify_formats_count = countof(supported_justify_formats),
    .sample_rates_list = supported_rates,
    .sample_rates_count = countof(supported_rates),
    .bits_per_sample_list = supported_bits_per_sample,
    .bits_per_sample_count = countof(supported_bits_per_sample),
};

enum {
    COMPONENT_PDEV,
    COMPONENT_I2C,
    COMPONENT_COUNT,
};

} // namespace

namespace audio {

zx_status_t Tas5805::Bind() {
    auto status = DdkAdd("tas5805");
    return status;
}

zx_status_t Tas5805::Create(zx_device_t* parent) {
    zx_status_t status;
    composite_protocol_t composite;

    status = device_get_protocol(parent, ZX_PROTOCOL_COMPOSITE, &composite);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Could not get composite protocol\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_device_t* components[COMPONENT_COUNT] = {};
    size_t actual;
    composite_get_components(&composite, components, countof(components), &actual);
    // Only PDEV and I2C components are required.
    if (actual < 2) {
        zxlogf(ERROR, "could not get components\n");
        return ZX_ERR_NOT_SUPPORTED;
    }


    fbl::AllocChecker ac;
    auto dev = std::unique_ptr<Tas5805>(new (&ac) Tas5805(parent, components[COMPONENT_I2C]));
    status = dev->Bind();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the memory for dev
    dev.release();
    return ZX_OK;
}


zx_status_t Tas5805::Reset() {
    return WriteReg(kRegReset, kRegResetBitCtrl);
}

void Tas5805::CodecGetDaiFormats(codec_get_dai_formats_callback callback, void* cookie) {
    callback(cookie, ZX_OK, &kSupportedDaiFormats);
}

void Tas5805::CodecSetDaiFormat(const dai_format_t* format,
                                codec_set_dai_format_callback callback, void* cookie) {
    if (format == nullptr) {
        callback(cookie, ZX_ERR_INVALID_ARGS);
        return;
    }
    size_t i = 0;
    // for (i = 0; i < format->lanes_count; ++i) {
    //     if (format->lanes_list[i] > kSupportedDaiFormats.n_lanes) {
    //         return ZX_ERR_NOT_SUPPORTED;
    //     }
    // }
    // for (i = 0; i < format->channels_count; ++i) {
    //     if (format->channels_list[i] > kSupportedDaiFormats.n_channels) {
    //         return ZX_ERR_NOT_SUPPORTED;
    //     }
    // }
    for (i = 0; i < kSupportedDaiFormats.sample_rates_count; ++i) {
        if (format->sample_rate == kSupportedDaiFormats.sample_rates_list[i]) {
            break;
        }
    }
    if (i == kSupportedDaiFormats.sample_rates_count) {
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }
    constexpr uint8_t configs[][2] = {
        {kRegSapCtrl1, kRegSapCtrl1Bits32bits},
    };
    for (auto& i : configs) {
        zx_status_t status = WriteReg(i[0], i[1]);
        if (status != ZX_OK) {
            callback(cookie, ZX_ERR_INTERNAL);
            return;
        }
    }
    callback(cookie, ZX_OK);
}

bool Tas5805::ValidGain(float gain) const {
    return (gain <= kMaxGain) && (gain >= kMinGain);
}

void Tas5805::CodecInitialize(codec_initialize_callback callback, void* cookie) {
    zx_status_t status = ZX_OK;
    constexpr uint8_t defaults[][2] = {
        {kRegSelectPage, 0x00},
        {kRegDeviceCtrl1, kRegDeviceCtrl1BitsPbtlMode | kRegDeviceCtrl1Bits1SpwMode},
    };
    for (auto& i : defaults) {
        status = WriteReg(i[0], i[1]);
        if (status != ZX_OK) {
            callback(cookie, status);
            return;
        }
    }
    status = ExitStandby();
    callback(cookie, status);
}

void Tas5805::CodecGetGainFormat(codec_get_gain_format_callback callback, void* cookie) {
    gain_format_t format;
    format.type = GAIN_TYPE_DECIBELS;
    format.min_gain = kMinGain;
    format.max_gain = kMaxGain;
    format.gain_step = kGainStep;
    callback(cookie, ZX_OK, &format);
}


void Tas5805::CodecSetGain(float gain, codec_set_gain_callback callback, void* cookie) {
    gain = std::clamp(gain, kMinGain, kMaxGain);
    uint8_t gain_reg = static_cast<uint8_t>(48 - gain * 2);
    zx_status_t status = WriteReg(kRegDigitalVol, gain_reg);
    if (status != ZX_OK) {
        callback(cookie, status);
        return;
    }
    current_gain_ = gain;
    callback(cookie, ZX_OK);
}

void Tas5805::CodecGetGain(codec_get_gain_callback callback, void* cookie) {
    callback(cookie, ZX_OK, current_gain_);
}

zx_status_t Tas5805::Standby() {
    return WriteReg(kRegDeviceCtrl2, kRegDeviceCtrl2BitsDeepSleep);
}

zx_status_t Tas5805::ExitStandby() {
    return WriteReg(kRegDeviceCtrl2, kRegDeviceCtrl2BitsPlay);
}

zx_status_t Tas5805::WriteReg(uint8_t reg, uint8_t value) {
    uint8_t write_buf[2];
    write_buf[0] = reg;
    write_buf[1] = value;
//#define TRACE_I2C
#ifdef TRACE_I2C
    printf("Writing register 0x%02X to value 0x%02X\n", reg, value);
    auto status = i2c_.WriteSync(write_buf, 2);
    if (status != ZX_OK) {
        printf("Could not I2C write %d\n", status);
        return status;
    }
    uint8_t buffer = 0;
    i2c_.ReadSync(reg, &buffer, 1);
    if (status != ZX_OK) {
        printf("Could not I2C read %d\n", status);
        return status;
    }
    printf("Read register 0x%02X, value 0x%02X\n", reg, buffer);
    return ZX_OK;
#else
    return i2c_.WriteSync(write_buf, 2);
#endif
}

zx_status_t tas5805_bind(void* ctx, zx_device_t* parent) {
    return Tas5805::Create(parent);
}

static zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops;
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = tas5805_bind;
    return ops;
}();

} // namespace audio

// clang-format off
ZIRCON_DRIVER_BEGIN(ti_tas5805, audio::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TI),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_TI_TAS5805),
ZIRCON_DRIVER_END(ti_tas5805)
// clang-format on
