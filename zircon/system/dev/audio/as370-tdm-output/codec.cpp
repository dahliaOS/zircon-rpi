// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "codec.h"

namespace audio {
namespace as370 {

zx_status_t Codec::GetGainFormat(gain_format_t* format) {
    struct AsyncOut {
        sync_completion_t completion;
        gain_format_t format;
    } out;
    proto_client_.GetGainFormat(
        [](void* ctx, const gain_format_t* format) {
            auto* out = reinterpret_cast<AsyncOut*>(ctx);
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

zx_status_t Codec::GetGainState(gain_state_t* state) {
    struct AsyncOut {
        sync_completion_t completion;
        gain_state_t state;
    } out;
    proto_client_.GetGainState(
        [](void* ctx, const gain_state_t* state) {
            auto* out = reinterpret_cast<AsyncOut*>(ctx);
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

zx_status_t Codec::SetGainState(gain_state_t* state) {
    proto_client_.SetGainState(state, [](void* ctx) {}, nullptr);
    return ZX_OK;
}

} // namespace as370
} // namespace audio
