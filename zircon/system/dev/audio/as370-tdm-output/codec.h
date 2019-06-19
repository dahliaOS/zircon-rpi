// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/debug.h>
#include <ddktl/protocol/codec.h>
#include <lib/sync/completion.h>
#include <lib/zx/time.h>

namespace audio {
namespace as370 {

struct Codec {
    static constexpr uint32_t kCodecTimeoutSecs = 1;

    struct AsyncOut {
        sync_completion_t completion;
        zx_status_t status;
    };

    zx_status_t GetGainFormat(gain_format_t* format);
    zx_status_t GetGainState(gain_state_t* state);
    zx_status_t SetGainState(gain_state_t* state);

    ddk::CodecProtocolClient proto_client_;
};

} // namespace as370
} // namespace audio
