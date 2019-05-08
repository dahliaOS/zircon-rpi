// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tas5805.h"

#include <optional>

#include <fbl/array.h>
#include <fbl/vector.h>
#include <lib/mock-i2c/mock-i2c.h>
#include <lib/sync/completion.h>
#include <zxtest/zxtest.h>

namespace audio {

TEST(Tas5805Test, BadSetDai) {
    mock_i2c::MockI2c mock_i2c;
    ddk::I2cChannel i2c(mock_i2c.GetProto());
    Tas5805 device(nullptr, std::move(i2c));

    // EXPECT_EQ(ZX_ERR_INVALID_ARGS, device.CodecSetDaiFormat(nullptr));

    // dai_format format = {};
    // format.sample_format = SAMPLE_FORMAT_FORMAT_PCM;
    // EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, device.CodecSetDaiFormat(&format));
    mock_i2c.VerifyAndClear();
}

TEST(Tas5805Test, GoodSetDai) {
    mock_i2c::MockI2c mock_i2c;
    ddk::I2cChannel i2c(mock_i2c.GetProto());
    Tas5805 device(nullptr, std::move(i2c));

    mock_i2c.ExpectWriteStop({0x33, 0x03});  // 32 bits.
    uint32_t lanes[] = { 0 };
    uint32_t channels[] = { 0, 1 };
    dai_format_t format = {};
    // clang-format off
    format.lanes_list      = lanes;
    format.lanes_count     = countof(lanes);
    format.channels_list   = channels;
    format.channels_count  = countof(channels);
    format.sample_format   = SAMPLE_FORMAT_FORMAT_I2S;
    format.justify_format  = JUSTIFY_FORMAT_JUSTIFY_I2S;
    format.sample_rate     = 48000;
    format.bits_per_sample = 32;
    format.sample_format = SAMPLE_FORMAT_FORMAT_I2S;
    struct AsyncOut {
        sync_completion_t completion;
        zx_status_t status;
    } out;
    device.CodecSetDaiFormat(
        &format, [](void* ctx, zx_status_t s) {
                     AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
                     out->status = s;
                     sync_completion_signal(&out->completion);
                 }, &out);
    EXPECT_OK(sync_completion_wait(&out.completion, zx::sec(1).get()));
    EXPECT_OK(out.status);
    mock_i2c.VerifyAndClear();
}

TEST(Tas5805Test, GetDai) {
    mock_i2c::MockI2c mock_i2c;
    ddk::I2cChannel i2c(mock_i2c.GetProto());
    Tas5805 device(nullptr, std::move(i2c));
    struct AsyncOut {
        sync_completion_t completion;
        dai_available_formats_t formats;
        fbl::Array<uint32_t> rates;
        zx_status_t status;
    } out;
    device.CodecGetDaiFormats(
        [](void* ctx, zx_status_t s, const dai_available_formats_t* formats) {
            AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
            out->formats = *formats;
            out->rates = fbl::Array(new uint32_t[formats->sample_rates_count],
                                            formats->sample_rates_count);
            memcpy(out->rates.get(), formats->sample_rates_list,
                   formats->sample_rates_count * sizeof(uint32_t));
            out->formats.sample_rates_list =
                reinterpret_cast<uint32_t*>(out->rates.get());
            out->status = s;
            sync_completion_signal(&out->completion);
        }, &out);
    EXPECT_OK(sync_completion_wait(&out.completion, zx::sec(1).get()));
    EXPECT_OK(out.status);
    EXPECT_EQ(out.formats.n_lanes, 1);
    EXPECT_EQ(out.formats.sample_formats_count, 1);
    EXPECT_EQ(out.formats.sample_formats_list[0], SAMPLE_FORMAT_FORMAT_I2S);
    EXPECT_EQ(out.formats.justify_formats_count, 1);
    EXPECT_EQ(out.formats.justify_formats_list[0], JUSTIFY_FORMAT_JUSTIFY_I2S);
    EXPECT_EQ(out.formats.sample_rates_count, 1);
    EXPECT_EQ(out.formats.sample_rates_list[0], 48000);
    EXPECT_EQ(out.formats.bits_per_sample_count, 1);
    EXPECT_EQ(out.formats.bits_per_sample_list[0], 32);
}

TEST(Tas5805Test, Init) {
    mock_i2c::MockI2c mock_i2c;
    mock_i2c
        .ExpectWriteStop({0x00, 0x00})  // Page 0.
        .ExpectWriteStop({0x02, 0x05})  // Stereo.
        .ExpectWriteStop({0x03, 0x03}); // Exit standby.

    ddk::I2cChannel i2c(mock_i2c.GetProto());
    Tas5805 device(nullptr, std::move(i2c));

    struct AsyncOut {
        sync_completion_t completion;
        zx_status_t status;
    } out;

    device.CodecInitialize(
        [](void* ctx, zx_status_t s) {
            AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
            out->status = s;
            sync_completion_signal(&out->completion);
        }, &out);
    EXPECT_OK(sync_completion_wait(&out.completion, zx::sec(1).get()));
    EXPECT_OK(out.status);
    mock_i2c.VerifyAndClear();
}
} // namespace audio
