// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_DRIVER_FIDL_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_DRIVER_FIDL_H_

#include <fuchsia/hardware/audio/cpp/fidl.h>
#include <fuchsia/media/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <zircon/device/audio.h>

#include <mutex>
#include <string>

#include "src/media/audio/audio_core/audio_device.h"
#include "src/media/audio/audio_core/audio_device_settings.h"
#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/ring_buffer.h"
#include "src/media/audio/audio_core/utils.h"

namespace media::audio {

namespace audio_fidl = ::fuchsia::hardware::audio;

class AudioDriverFidl : public AudioDriver {
 public:
  AudioDriverFidl(AudioDevice* owner) : AudioDriver(owner) {}

  using DriverTimeoutHandler = fit::function<void(zx::duration)>;
  AudioDriverFidl(AudioDevice* owner, DriverTimeoutHandler timeout_handler)
      : AudioDriver(owner, std::move(timeout_handler)) {}

  virtual ~AudioDriverFidl() = default;

  zx_status_t Init(zx::channel stream_channel) override;
  zx_status_t GetDriverInfo() override;
  zx_status_t Configure(const Format& format, zx::duration min_ring_buffer_duration) override;
  zx_status_t Start() override;
  zx_status_t Stop() override;
  zx_status_t SetPlugDetectEnabled(bool enabled) override;
  zx_status_t SendSetGain(const AudioDeviceSettings::GainState& gain_state,
                          audio_set_gain_flags_t set_flags) override;
  zx_status_t SelectBestFormat(uint32_t* frames_per_second_inout, uint32_t* channels_inout,
                               fuchsia::media::AudioSampleFormat* sample_format_inout) override;

 private:
  const std::vector<driver_fidl::PcmSupportedFormats>& formats() const { return formats_; }
  // Evaluate each currently pending timeout. Program the command timeout timer appropriately.
  void SetupCommandTimeout() FXL_EXCLUSIVE_LOCKS_REQUIRED(owner_->mix_domain().token()) override;
  zx_status_t OnDriverInfoFetched(uint32_t info)
      FXL_EXCLUSIVE_LOCKS_REQUIRED(owner_->mix_domain().token()) override;

  // Plug detection state.
  bool pd_hardwired_ = false;
  std::vector<driver_fidl::PcmSupportedFormats> formats_;
  fidl::InterfacePtr<driver_fidl::StreamConfig> stream_config_intf_;
  fidl::InterfacePtr<driver_fidl::RingBuffer> ring_buffer_intf_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_DRIVER_FIDL_H_
