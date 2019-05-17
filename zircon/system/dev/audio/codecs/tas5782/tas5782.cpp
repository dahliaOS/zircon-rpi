// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tas5782.h"

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
// TODO(andresoportus): Add handling for the other formats supported by this codec.
static const uint32_t supported_n_channels[] = {2};
static const sample_format_t supported_sample_formats[] = {SAMPLE_FORMAT_PCM_SIGNED};
static const justify_format_t supported_justify_formats[] = {JUSTIFY_FORMAT_JUSTIFY_I2S};
static const uint32_t supported_rates[] = {48000};
static const uint8_t supported_bits_per_sample[] = {32};
static const dai_supported_formats_t kSupportedDaiFormats = {
    .number_of_channels_list = supported_n_channels,
    .number_of_channels_count = countof(supported_n_channels),
    .types_list = supported_sample_formats,
    .types_count = countof(supported_sample_formats),
    .justify_formats_list = supported_justify_formats,
    .justify_formats_count = countof(supported_justify_formats),
    .frame_rates_list = supported_rates,
    .frame_rates_count = countof(supported_rates),
    .bits_per_channel_list = supported_bits_per_sample,
    .bits_per_channel_count = countof(supported_bits_per_sample),
    .bits_per_sample_list = supported_bits_per_sample,
    .bits_per_sample_count = countof(supported_bits_per_sample),
};

enum {
    COMPONENT_PDEV,
    COMPONENT_I2C,
    COMPONENT_RESET_GPIO,
    COMPONENT_MUTE_GPIO,
    COMPONENT_COUNT,
};

} // namespace

namespace audio {

zx_status_t Tas5782::Bind() {
    codec_reset_.Write(0); // Reset.
    // Delay to be safe.
    zx_nanosleep(zx_deadline_after(zx::usec(1).get()));
    codec_reset_.Write(1); // Set to "not reset".
    // Delay to be safe.
    zx_nanosleep(zx_deadline_after(zx::msec(10).get()));

    codec_mute_.Write(1); // Set to "unmute".

    zx_status_t status = ZX_OK;
    constexpr uint8_t defaults[][2] = {
        {0x13, 0x10}, // The PLL reference clock is SCLK.
        {0x04, 0x01}, // PLL for MCLK setting.
        {0x40, 0x03}, // I2S, 32 bits.
        {0x42, 0x22}, // Left DAC to Left ch, Right DAC to right channel.
    };
    for (auto& i : defaults) {
        status = WriteReg(i[0], i[1]);
        if (status != ZX_OK) {
            return status;
        }
    }
    status = ExitStandby();
    if (status != ZX_OK) {
        return status;
    }
    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_TI},
        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_TI_TAS5782},
    };
    return DdkAdd("tas5782", 0, props, countof(props));
}

zx_status_t Tas5782::Create(zx_device_t* parent) {
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
    auto dev = std::unique_ptr<Tas5782>(new (&ac) Tas5782(parent,
                                                          components[COMPONENT_I2C],
                                                          components[COMPONENT_RESET_GPIO],
                                                          components[COMPONENT_RESET_GPIO]));
    status = dev->Bind();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the memory for dev.
    dev.release();
    return ZX_OK;
}

zx_status_t Tas5782::Reset() {
    return WriteReg(0x01, 0x01);
}

void Tas5782::CodecSetDaiFormat(const dai_format_t* format,
                                codec_set_dai_format_callback callback, void* cookie) {
    if (format == nullptr) {
        callback(cookie, ZX_ERR_INVALID_ARGS);
        return;
    }

    // Only allow one lane and 2 channels.
    if (format->channels_to_use_count != 2 || format->channels_to_use_list[0] != 0 ||
        format->channels_to_use_list[0] != 1) {
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }

    // Only I2S.
    if (format->sample_type != SAMPLE_TYPE_SIGNED_PCM ||
        format->justify_format != JUSTIFY_FORMAT_JUSTIFY_I2S) {
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }

    // Check rates allowed.
    size_t i = 0;
    for (i = 0; i < kSupportedDaiFormats.frame_rates_count; ++i) {
        if (format->frame_rate == kSupportedDaiFormats.frame_rates_list[i]) {
            break;
        }
    }
    if (i == kSupportedDaiFormats.frame_rates_count) {
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }

    // Allow 32 bits only.
    if (format->bits_per_sample != 32) {
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }
    callback(cookie, ZX_OK);
}

bool Tas5782::ValidGain(float gain) const {
    return (gain <= kMaxGain) && (gain >= kMinGain);
}

void Tas5782::CodecGetGainFormat(codec_get_gain_format_callback callback, void* cookie) {
    gain_format_t format;
    format.type = GAIN_TYPE_DECIBELS;
    format.min_gain = kMinGain;
    format.max_gain = kMaxGain;
    format.gain_step = kGainStep;
    callback(cookie, ZX_OK, &format);
}

void Tas5782::CodecSetGain(float gain, codec_set_gain_callback callback, void* cookie) {
    gain = std::clamp(gain, kMinGain, kMaxGain);
    uint8_t gain_reg = static_cast<uint8_t>(48 - gain * 2);
    zx_status_t status;
    status = WriteReg(61, gain_reg); // Left gain.
    if (status != ZX_OK) {
        callback(cookie, status);
        return;
    }
    status = WriteReg(62, gain_reg); // Right gain.
    if (status != ZX_OK) {
        callback(cookie, status);
        return;
    }
    current_gain_ = gain;
    callback(cookie, ZX_OK);
}

void Tas5782::CodecGetGain(codec_get_gain_callback callback, void* cookie) {
    callback(cookie, ZX_OK, current_gain_);
}

zx_status_t Tas5782::Standby() {
    return WriteReg(0x02, 0x10);
}

zx_status_t Tas5782::ExitStandby() {
    return WriteReg(0x02, 0x00);
}

zx_status_t Tas5782::WriteReg(uint8_t reg, uint8_t value) {
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
    printf("Read register just written 0x%02X, value 0x%02X\n", reg, buffer);
    return ZX_OK;
#else
    return i2c_.WriteSync(write_buf, 2);
#endif
}

zx_status_t tas5782_bind(void* ctx, zx_device_t* parent) {
    return Tas5782::Create(parent);
}

static zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops;
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = tas5782_bind;
    return ops;
}();

} // namespace audio

// clang-format off
ZIRCON_DRIVER_BEGIN(ti_tas5782, audio::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TI),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_TI_TAS5782),
ZIRCON_DRIVER_END(ti_tas5782)
// clang-format on

