// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tas5782.h"

#include <optional>

#include <fbl/array.h>
#include <fbl/vector.h>
#include <lib/mock-gpio/mock-gpio.h>
#include <lib/mock-i2c/mock-i2c.h>
#include <lib/sync/completion.h>
#include <zxtest/zxtest.h>

namespace audio {

struct Tas5782Sync : public Tas5782 {
    explicit Tas5782Sync(zx_device_t* device, const ddk::I2cChannel& i2c,
                         const ddk::GpioProtocolClient& codec_reset,
                         const ddk::GpioProtocolClient& codec_mute)
        : Tas5782(device, i2c, codec_reset, codec_mute) {}
    zx_status_t CodecSetDaiFormat(dai_format_t* format) {
        struct AsyncOut {
            sync_completion_t completion;
            zx_status_t status;
        } out;
        Tas5782::CodecSetDaiFormat(
            format, [](void* ctx, zx_status_t s) {
                AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
                out->status = s;
                sync_completion_signal(&out->completion);
            },
            &out);
        auto status = sync_completion_wait(&out.completion, zx::sec(1).get());
        if (status != ZX_OK) {
            return status;
        }
        return out.status;
    }
};

TEST(Tas5782Test, GoodSetDai) {
    mock_i2c::MockI2c mock_i2c;
    ddk::I2cChannel i2c(mock_i2c.GetProto());
    ddk::GpioProtocolClient unused_gpio0, unused_gpio1;
    Tas5782Sync device(nullptr, std::move(i2c), unused_gpio0, unused_gpio1);

    uint32_t lanes[] = {0};
    uint32_t channels[] = {0, 1};
    dai_format_t format = {};
    format.lanes_list = lanes;
    format.lanes_count = countof(lanes);
    format.channels_list = channels;
    format.channels_count = countof(channels);
    format.sample_format = SAMPLE_FORMAT_FORMAT_I2S;
    format.justify_format = JUSTIFY_FORMAT_JUSTIFY_I2S;
    format.sample_rate = 48000;
    format.bits_per_sample = 32;
    EXPECT_OK(device.CodecSetDaiFormat(&format));
}


TEST(Tas5782Test, BadSetDai) {
    mock_i2c::MockI2c mock_i2c;
    ddk::I2cChannel i2c(mock_i2c.GetProto());
    ddk::GpioProtocolClient unused_gpio0, unused_gpio1;
    Tas5782Sync device(nullptr, std::move(i2c), unused_gpio0, unused_gpio1);

    // No format at all.
    EXPECT_EQ(ZX_ERR_INVALID_ARGS, device.CodecSetDaiFormat(nullptr));

    // Blank format.
    dai_format format = {};
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, device.CodecSetDaiFormat(&format));

    // Almost good format (wrong justify_format).
    uint32_t lanes[] = {0};
    uint32_t channels[] = {0, 1};
    format.lanes_list = lanes;
    format.lanes_count = countof(lanes);
    format.channels_list = channels;
    format.channels_count = countof(channels);
    format.sample_format = SAMPLE_FORMAT_FORMAT_I2S;
    format.justify_format = JUSTIFY_FORMAT_JUSTIFY_LEFT; // This must fail, only I2S supported.
    format.sample_rate = 48000;
    format.bits_per_sample = 16;
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, device.CodecSetDaiFormat(&format));

    // Almost good format (wrong channels).
    format.sample_format = SAMPLE_FORMAT_FORMAT_I2S; // Restore I2S sample format.
    format.channels_count = 1;
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, device.CodecSetDaiFormat(&format));

    // Almost good format (wrong rate).
    format.channels_count = 2; // Restore channel count;
    format.sample_rate = 44100;
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, device.CodecSetDaiFormat(&format));

    mock_i2c.VerifyAndClear();
}

TEST(Tas5782Test, GetDai) {
    mock_i2c::MockI2c mock_i2c;
    ddk::I2cChannel i2c(mock_i2c.GetProto());
    ddk::GpioProtocolClient unused_gpio0, unused_gpio1;
    Tas5782 device(nullptr, std::move(i2c), unused_gpio0, unused_gpio1);
    struct AsyncOut {
        sync_completion_t completion;
        dai_available_formats_t formats;
        fbl::Array<uint32_t> rates;
        zx_status_t status;
    } out;

    device.CodecGetDaiFormats(
        [](void* ctx, zx_status_t status, const dai_available_formats_t* formats) {
            AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
            out->formats = *formats;
            out->rates = fbl::Array(new uint32_t[formats->sample_rates_count],
                                    formats->sample_rates_count);
            memcpy(out->rates.get(), formats->sample_rates_list,
                   formats->sample_rates_count * sizeof(uint32_t));
            out->formats.sample_rates_list =
                reinterpret_cast<uint32_t*>(out->rates.get());
            out->status = status;
            sync_completion_signal(&out->completion);
        },
        &out);
    EXPECT_OK(sync_completion_wait(&out.completion, zx::sec(1).get()));
    EXPECT_OK(out.status);
    EXPECT_EQ(out.formats.n_lanes, 1);
    EXPECT_EQ(out.formats.n_channels, 2);
    EXPECT_EQ(out.formats.sample_formats_count, 1);
    EXPECT_EQ(out.formats.sample_formats_list[0], SAMPLE_FORMAT_FORMAT_I2S);
    EXPECT_EQ(out.formats.justify_formats_count, 1);
    EXPECT_EQ(out.formats.justify_formats_list[0], JUSTIFY_FORMAT_JUSTIFY_I2S);
    EXPECT_EQ(out.formats.sample_rates_count, 1);
    EXPECT_EQ(out.formats.sample_rates_list[0], 48000);
    EXPECT_EQ(out.formats.bits_per_sample_count, 1);
    EXPECT_EQ(out.formats.bits_per_sample_list[0], 32);
}

TEST(Tas5782Test, Init) {
    mock_i2c::MockI2c mock_i2c;
    mock_i2c
        .ExpectWriteStop({0x13, 0x10})  // The PLL reference clock is SCLK.
        .ExpectWriteStop({0x04, 0x01})  // PLL for MCLK setting.
        .ExpectWriteStop({0x40, 0x03})  // I2S, 32 bits.
        .ExpectWriteStop({0x42, 0x22})  // Left DAC to Left ch, Right DAC to right channel.
        .ExpectWriteStop({0x02, 0x00}); // Exit standby.

    ddk::I2cChannel i2c(mock_i2c.GetProto());
    mock_gpio::MockGpio mock_gpio0, mock_gpio1;
    ddk::GpioProtocolClient gpio0(mock_gpio0.GetProto());
    ddk::GpioProtocolClient gpio1(mock_gpio1.GetProto());
    mock_gpio0.ExpectWrite(ZX_OK, 0).ExpectWrite(ZX_OK, 1); // Reset, set to 0 and then to 1.
    mock_gpio1.ExpectWrite(ZX_OK, 1);                       // Set to "unmute".

    Tas5782 device(nullptr, std::move(i2c), gpio0, gpio1);

    struct AsyncOut {
        sync_completion_t completion;
        zx_status_t status;
    } out;

    device.CodecInitialize(
        [](void* ctx, zx_status_t s) {
            AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
            out->status = s;
            sync_completion_signal(&out->completion);
        },
        &out);
    EXPECT_OK(sync_completion_wait(&out.completion, zx::sec(1).get()));
    EXPECT_OK(out.status);
    mock_i2c.VerifyAndClear();
    mock_gpio0.VerifyAndClear();
    mock_gpio1.VerifyAndClear();
}
} // namespace audio
