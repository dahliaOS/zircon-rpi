// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <optional>

#include <ddk/debug.h>
#include <ddktl/protocol/codec.h>
#include <lib/sync/completion.h>
#include <lib/zx/time.h>

namespace audio {
namespace mt8167 {

static constexpr sample_format_t wanted_sample_format = SAMPLE_FORMAT_PCM_SIGNED;
static constexpr justify_format_t wanted_justify_format = JUSTIFY_FORMAT_JUSTIFY_I2S;
static constexpr uint32_t wanted_frame_rate = 48000;
static constexpr uint8_t wanted_bits_per_sample = 32;

static bool IsFormatSupported(sample_format_t sample_format, justify_format_t justify_format,
                              uint32_t frame_rate, uint8_t bits_per_sample,
                              const dai_supported_formats_t* formats) {
    printf("%s\n", __func__);
    size_t i = 0;
    for (i = 0; i < formats->sample_formats_count &&
                formats->sample_formats_list[i] != sample_format;
         ++i) {
    }
    if (i == formats->sample_formats_count) {
        zxlogf(ERROR, "%s did not find wanted sample format\n", __FILE__);
        return false;
    }
    for (i = 0; i < formats->justify_formats_count &&
                formats->justify_formats_list[i] != justify_format;
         ++i) {
    }
    if (i == formats->justify_formats_count) {
        zxlogf(ERROR, "%s did not find wanted justify format\n", __FILE__);
        return false;
    }
    for (i = 0; i < formats->frame_rates_count &&
                formats->frame_rates_list[i] != frame_rate;
         ++i) {
    }
    if (i == formats->frame_rates_count) {
        zxlogf(ERROR, "%s did not find wanted sample rate\n", __FILE__);
        return false;
    }
    for (i = 0; i < formats->bits_per_sample_count &&
                formats->bits_per_sample_list[i] != bits_per_sample;
         ++i) {
    }
    if (i == formats->bits_per_sample_count) {
        zxlogf(ERROR, "%s did not find wanted bits per sample\n", __FILE__);
        return false;
    }
    return true;
}

struct Codec {
    static constexpr uint32_t kCodecTimeoutSecs = 1;

    struct AsyncOut {
        sync_completion_t completion;
        zx_status_t status;
    };

    zx_status_t CheckExpectedDaiFormat() {
        printf("%s\n", __func__);
        AsyncOut out;
        proto_client_.GetDaiFormats(
            [](void* ctx, zx_status_t s, const dai_supported_formats_t* formats_list,
               size_t formats_count) {
                AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
                out->status = s;
                if (out->status == ZX_OK) {
                    size_t i = 0;
                    for (; i < formats_count; ++i) {
                        if (IsFormatSupported(
                                wanted_sample_format, wanted_justify_format, wanted_frame_rate,
                                wanted_bits_per_sample, &formats_list[i])) {
                            break;
                        }
                    }
                    out->status = i != formats_count ? ZX_OK : ZX_ERR_INTERNAL;
                }
                sync_completion_signal(&out->completion);
            },
            &out);
        auto status = sync_completion_wait(&out.completion, zx::sec(kCodecTimeoutSecs).get());
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s failed to get DAI formats %d\n", __FUNCTION__, status);
            return status;
        }
        if (out.status != ZX_OK) {
            zxlogf(ERROR, "%s did not find expected DAI formats %d\n", __FUNCTION__, out.status);
        }
        return status;
    }

    zx_status_t SetDaiFormat(dai_format_t format) {
        AsyncOut out;
        proto_client_.SetDaiFormat(
            &format, [](void* ctx, zx_status_t s) {
                AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
                out->status = s;
                sync_completion_signal(&out->completion);
            },
            &out);
        auto status = sync_completion_wait(&out.completion, zx::sec(kCodecTimeoutSecs).get());
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s failed to get DAI formats %d\n", __FUNCTION__, status);
            return status;
        }
        if (out.status != ZX_OK) {
            zxlogf(ERROR, "%s did not find expected DAI formats %d\n", __FUNCTION__, out.status);
        }
        return status;
    }

    zx_status_t GetGainFormat(gain_format_t* format) {
        struct AsyncOut {
            sync_completion_t completion;
            gain_format_t format;
        } out;
        proto_client_.GetGainFormat(
            [](void* ctx, const gain_format_t* format) {
                AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
                out->format = *format;
                sync_completion_signal(&out->completion);
            },
            &out);
        auto status = sync_completion_wait(&out.completion, zx::sec(kCodecTimeoutSecs).get());
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s failed to get gain format %d\n", __FUNCTION__, status);
        }
        *format = out.format;
        return status;
    }

    zx_status_t GetGainState(gain_state_t* state) {
        struct AsyncOut {
            sync_completion_t completion;
            gain_state_t state;
        } out;
        proto_client_.GetGainState(
            [](void* ctx, const gain_state_t* state) {
                AsyncOut* out = reinterpret_cast<AsyncOut*>(ctx);
                out->state = *state;
                sync_completion_signal(&out->completion);
            },
            &out);
        auto status = sync_completion_wait(&out.completion, zx::sec(kCodecTimeoutSecs).get());
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s failed to get gain state %d\n", __FUNCTION__, status);
        }
        *state = out.state;
        return status;
    }

    zx_status_t SetGainState(gain_state_t* state) {
        proto_client_.SetGainState(state, [](void* ctx) {}, nullptr);
        return ZX_OK;
    }

    ddk::CodecProtocolClient proto_client_;
};

} // namespace mt8167
} // namespace audio
