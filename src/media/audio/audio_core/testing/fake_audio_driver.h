// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DRIVER_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DRIVER_H_

#include <fuchsia/hardware/audio/cpp/fidl.h>
#include <lib/async/cpp/time.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <zircon/device/audio.h>

#include <cstring>
#include <optional>

namespace media::audio::testing {

class FakeAudioDriver : public fuchsia::hardware::audio::StreamConfig,
                        public fuchsia::hardware::audio::RingBuffer {
 public:
  FakeAudioDriver(zx::channel channel, async_dispatcher_t* dispatcher);

  fzl::VmoMapper CreateRingBuffer(size_t size);
  void Start();
  void Stop();

  void set_stream_unique_id(const audio_stream_unique_id_t& uid) {
    std::memcpy(uid_.data, uid.data, sizeof(uid.data));
  }
  void set_device_manufacturer(std::string mfgr) { manufacturer_ = std::move(mfgr); }
  void set_device_product(std::string product) { product_ = std::move(product); }
  void set_gain(float gain) { cur_gain_ = gain; }
  void set_gain_limits(float min_gain, float max_gain) {
    gain_limits_ = std::make_pair(min_gain, max_gain);
  }
  void set_can_agc(bool can_agc) { can_agc_ = can_agc; }
  void set_cur_agc(bool cur_agc) { cur_agc_ = cur_agc; }
  void set_can_mute(bool can_mute) { can_mute_ = can_mute; }
  void set_cur_mute(bool cur_mute) { cur_mute_ = cur_mute; }
  void set_formats(fuchsia::hardware::audio::PcmSupportedFormats formats) {
    formats_ = std::move(formats);
  }
  void set_clock_domain(int32_t clock_domain) { clock_domain_ = clock_domain; }
  void set_plugged(bool plugged) { plugged_ = plugged; }
  void set_fifo_depth(uint32_t fifo_depth) { fifo_depth_ = fifo_depth; }

  // |true| after an |audio_rb_cmd_start| is received, until an |audio_rb_cmd_stop| is received.
  bool is_running() const { return is_running_; }

  // The 'selected format' for the driver.
  // The returned optional will be empty if no |CreateRingBuffer| command has been received.
  std::optional<fuchsia::hardware::audio::PcmFormat> selected_format() const {
    return selected_format_;
  }

 private:
  // fuchsia hardware audio StreamConfig Interface
  void GetProperties(
      fuchsia::hardware::audio::StreamConfig::GetPropertiesCallback callback) override;
  void GetSupportedFormats(
      fuchsia::hardware::audio::StreamConfig::GetSupportedFormatsCallback callback) override;
  void CreateRingBuffer(
      fuchsia::hardware::audio::Format format,
      ::fidl::InterfaceRequest<fuchsia::hardware::audio::RingBuffer> ring_buffer) override;
  void WatchGainState(
      fuchsia::hardware::audio::StreamConfig::WatchGainStateCallback callback) override;
  void SetGain(fuchsia::hardware::audio::GainState target_state) override;
  void WatchPlugState(
      fuchsia::hardware::audio::StreamConfig::WatchPlugStateCallback callback) override;

  // fuchsia hardware audio RingBuffer Interface
  void GetProperties(fuchsia::hardware::audio::RingBuffer::GetPropertiesCallback callback) override;
  void WatchClockRecoveryPositionInfo(
      fuchsia::hardware::audio::RingBuffer::WatchClockRecoveryPositionInfoCallback callback)
      override;
  void GetVmo(uint32_t min_frames, uint32_t clock_recovery_notifications_per_ring,
              fuchsia::hardware::audio::RingBuffer::GetVmoCallback callback) override;
  void Start(fuchsia::hardware::audio::RingBuffer::StartCallback callback) override;
  void Stop(fuchsia::hardware::audio::RingBuffer::StopCallback callback) override;

  audio_stream_unique_id_t uid_ = {};
  std::string manufacturer_ = "default manufacturer";
  std::string product_ = "default product";
  float cur_gain_ = 0.0f;
  std::pair<float, float> gain_limits_{-160.0f, 3.0f};
  bool can_agc_ = true;
  bool cur_agc_ = false;
  bool can_mute_ = true;
  bool cur_mute_ = false;
  bool plug_state_sent_ = false;
  bool gain_state_sent_ = false;
  fuchsia::hardware::audio::PcmSupportedFormats formats_ = {};
  int32_t clock_domain_ = 0;
  size_t ring_buffer_size_;
  zx::vmo ring_buffer_;

  uint32_t fifo_depth_ = 0;
  bool plugged_ = true;

  std::optional<fuchsia::hardware::audio::PcmFormat> selected_format_;

  bool is_running_ = false;

  async_dispatcher_t* dispatcher_;
  fidl::Binding<fuchsia::hardware::audio::StreamConfig> stream_binding_;
  std::optional<fidl::Binding<fuchsia::hardware::audio::RingBuffer>> ring_buffer_binding_;
  fidl::InterfaceRequest<fuchsia::hardware::audio::StreamConfig> stream_req_;
  fidl::InterfaceRequest<fuchsia::hardware::audio::RingBuffer> ring_buffer_req_;
};

}  // namespace media::audio::testing

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DRIVER_H_
