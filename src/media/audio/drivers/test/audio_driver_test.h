// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_TEST_AUDIO_DRIVER_TEST_H_
#define SRC_MEDIA_AUDIO_DRIVERS_TEST_AUDIO_DRIVER_TEST_H_

#include <fuchsia/hardware/audio/cpp/fidl.h>
#include <fuchsia/media/cpp/fidl.h>
#include <lib/fzl/vmo-mapper.h>

#include "src/lib/fsl/io/device_watcher.h"
#include "src/media/audio/lib/test/message_transceiver.h"
#include "src/media/audio/lib/test/test_fixture.h"

namespace media::audio::test {

constexpr size_t kUniqueIdLength = 16;

enum DeviceType { Input, Output };

class AudioDriverTest : public TestFixture {
 protected:
  static void SetUpTestSuite();
  static zx_txid_t NextTransactionId();

  void TearDown() override;

  bool WaitForDevice(DeviceType device_type);
  void AddDevice(int dir_fd, const std::string& name, DeviceType device_type);

  void RequestStreamProperties();
  void RequestGain();
  void RequestSetGain();
  void RequestFormats();
  void SelectFirstFormat();
  void SelectLastFormat();
  void RequestRingBuffer();
  void RequestRingBufferMin();
  void RequestRingBufferMax();
  void RequestPlugDetect();

  void RequestRingBufferProperties();
  void RequestBuffer(uint32_t min_ring_buffer_frames, uint32_t notifications_per_ring);
  void RequestStart();
  void RequestStop();

  void CalculateFrameSize();

  void ExpectPositionNotifyCount(uint32_t count);
  void ExpectNoPositionNotifications();

 private:
  static std::atomic_uint32_t unique_transaction_id_;

  static bool no_input_devices_found_;
  static bool no_output_devices_found_;
  std::vector<std::unique_ptr<fsl::DeviceWatcher>> watchers_;

  DeviceType device_type_;

  fidl::InterfacePtr<fuchsia::hardware::audio::StreamConfig> stream_config_intf_;
  fidl::InterfacePtr<fuchsia::hardware::audio::RingBuffer> ring_buffer_intf_;

  bool stream_config_ready_ = false;
  bool ring_buffer_ready_ = false;

  std::array<uint8_t, kUniqueIdLength> unique_id_;
  std::string manufacturer_;
  std::string product_;

  bool cur_mute_ = false;
  bool can_mute_ = false;
  bool set_mute_ = false;

  bool cur_agc_ = false;
  bool can_agc_ = false;
  bool set_agc_ = false;

  float cur_gain_ = 0.0f;
  float min_gain_ = 0.0f;
  float max_gain_ = 0.0f;
  float gain_step_ = 0.0f;
  float set_gain_ = 0.0f;

  std::vector<fuchsia::hardware::audio::PcmSupportedFormats> pcm_formats_;

  uint16_t get_formats_range_count_ = 0;
  uint16_t next_format_range_ndx_ = 0;

  fuchsia::hardware::audio::PcmFormat pcm_format_;
  uint16_t frame_size_ = 0;

  fuchsia::hardware::audio::PlugDetectCapabilities plug_detect_capabilities_;
  bool plugged_ = false;
  zx_time_t plug_state_time_ = 0;

  uint64_t external_delay_nsec_ = 0;
  uint32_t fifo_depth_ = 0;
  uint32_t clock_domain_ = 0;
  bool needs_cache_flush_or_invalidate_ = false;

  uint32_t min_ring_buffer_frames_ = 0;
  uint32_t notifications_per_ring_ = 0;
  uint32_t ring_buffer_frames_ = 0;
  fzl::VmoMapper ring_buffer_;

  zx_time_t start_time_ = 0;

  fuchsia::hardware::audio::RingBufferPositionInfo position_info_ = {};

  bool received_get_stream_properties_ = false;
  bool received_get_gain_ = false;
  bool received_get_formats_ = false;

  bool format_is_set_ = false;
  bool received_plug_detect_ = false;

  bool received_get_ring_buffer_properties_ = false;
  bool received_get_buffer_ = false;
  bool received_start_ = false;
  bool received_stop_ = false;
  uint32_t position_notification_count_ = 0;
};

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_DRIVERS_TEST_AUDIO_DRIVER_TEST_H_
