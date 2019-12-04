// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/drivers/test/audio_driver_test.h"

#include <dirent.h>
#include <fcntl.h>
#include <lib/fdio/fdio.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/vmo.h>

#include <algorithm>
#include <cstring>

#include "src/media/audio/lib/logging/logging.h"

namespace media::audio::test {

static const struct {
  const char* path;
  DeviceType device_type;
} AUDIO_DEVNODES[] = {
    {.path = "/dev/class/audio-input-2", .device_type = DeviceType::Input},
    {.path = "/dev/class/audio-output-2", .device_type = DeviceType::Output},
};

// static
bool AudioDriverTest::no_input_devices_found_ = false;
bool AudioDriverTest::no_output_devices_found_ = false;

// static
void AudioDriverTest::SetUpTestSuite() {
  // For verbose logging, set to -media::audio::TRACE or -media::audio::SPEW
  Logging::Init(FX_LOG_INFO, {"audio_driver_test"});
}

// static
std::atomic_uint32_t AudioDriverTest::unique_transaction_id_ = 0;

void AudioDriverTest::TearDown() {
  watchers_.clear();

  TestFixture::TearDown();
}

bool AudioDriverTest::WaitForDevice(DeviceType device_type) {
  if (device_type == DeviceType::Input && AudioDriverTest::no_input_devices_found_) {
    return false;
  }
  if (device_type == DeviceType::Output && AudioDriverTest::no_output_devices_found_) {
    return false;
  }

  device_type_ = device_type;
  bool enumeration_done = false;

  // Set up the watchers, etc. If any fail, automatically stop monitoring all device sources.
  for (const auto& devnode : AUDIO_DEVNODES) {
    if (device_type != devnode.device_type) {
      continue;
    }

    auto watcher = fsl::DeviceWatcher::CreateWithIdleCallback(
        devnode.path,
        [this, device_type](int dir_fd, const std::string& filename) {
          AUD_VLOG(TRACE) << "'" << filename << "' dir_fd " << dir_fd;
          this->AddDevice(dir_fd, filename, device_type);
        },
        [&enumeration_done]() { enumeration_done = true; });

    if (watcher == nullptr) {
      EXPECT_FALSE(watcher == nullptr)
          << "AudioDriverTest failed to create DeviceWatcher for '" << devnode.path << "'.";
      watchers_.clear();
      return false;
    }
    watchers_.emplace_back(std::move(watcher));
  }
  //
  // ... or ...
  //
  // Receive a call to AddDeviceByChannel(std::move(stream_channel), name, device_type);
  //

  RunLoopUntil([&enumeration_done]() { return enumeration_done; });

  // If we timed out waiting for devices, this target may not have any. Don't waste further time.
  if (!stream_config_ready_) {
    if (device_type == DeviceType::Input) {
      AudioDriverTest::no_input_devices_found_ = true;
    } else {
      AudioDriverTest::no_output_devices_found_ = true;
    }
    FX_LOGS(WARNING) << "*** No audio " << ((device_type == DeviceType::Input) ? "input" : "output")
                     << " devices detected on this target. ***";
  }

  return stream_config_ready_;
}

void AudioDriverTest::AddDevice(int dir_fd, const std::string& name, DeviceType device_type) {
  // TODO(mpuryear): on systems with more than one audio device of a given type, test them all.
  if (stream_config_ready_) {
    FX_LOGS(WARNING) << "More than one device detected. For now, we need to ignore it.";
    return;
  }

  // Open the device node.
  fbl::unique_fd dev_node(openat(dir_fd, name.c_str(), O_RDONLY));
  if (!dev_node.is_valid()) {
    FX_PLOGS(ERROR, errno) << "AudioDriverTest failed to open device node at \"" << name << "\". ("
                           << strerror(errno) << " : " << errno << ")";
    FAIL();
  }

  // Obtain the FDIO device channel, wrap it in a sync proxy, use that to get the stream channel.
  zx::channel dev_channel;
  zx_status_t status =
      fdio_get_service_handle(dev_node.release(), dev_channel.reset_and_get_address());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to obtain FDIO service channel to audio "
                            << ((device_type == DeviceType::Input) ? "input" : "output");
    FAIL();
  }

  // Obtain the stream channel
  auto dev =
      fidl::InterfaceHandle<fuchsia::hardware::audio::Device>(std::move(dev_channel)).BindSync();
  fidl::InterfaceHandle<fuchsia::hardware::audio::StreamConfig> intf = {};

  status = dev->GetChannel(&intf);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to open channel to audio "
                            << ((device_type == DeviceType::Input) ? "input" : "output");
    FAIL();
  }
  auto channel = intf.TakeChannel();

  AUD_VLOG(TRACE) << "Successfully opened devnode '" << name << "' for audio "
                  << ((device_type == DeviceType::Input) ? "input" : "output");

  stream_config_intf_ =
      fidl::InterfaceHandle<fuchsia::hardware::audio::StreamConfig>(std::move(channel)).Bind();
  if (!stream_config_intf_.is_bound()) {
    FX_LOGS(ERROR) << "Failed to get stream channel";
    FAIL();
  }
  stream_config_intf_.set_error_handler([](zx_status_t status) {
    FX_PLOGS(ERROR, status) << "Test failed with error: " << status;
    FAIL();
  });

  stream_config_ready_ = true;
}

// Stream channel requests
//
// Request the stream properties including the driver's unique ID which must be unique between input
// and output.
// TODO(mpuryear): ensure that this differs between input and output.
void AudioDriverTest::RequestStreamProperties() {
  stream_config_intf_->GetProperties([this](fuchsia::hardware::audio::StreamProperties prop) {
    memcpy(unique_id_.data(), prop.unique_id().data(), kUniqueIdLength);

    char id_buf[2 * kUniqueIdLength + 1];
    std::snprintf(id_buf, sizeof(id_buf),
                  "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", unique_id_[0],
                  unique_id_[1], unique_id_[2], unique_id_[3], unique_id_[4], unique_id_[5],
                  unique_id_[6], unique_id_[7], unique_id_[8], unique_id_[9], unique_id_[10],
                  unique_id_[11], unique_id_[12], unique_id_[13], unique_id_[14], unique_id_[15]);
    AUD_VLOG(TRACE) << "Received unique_id " << id_buf;

    if (device_type_ == DeviceType::Input) {
      ASSERT_TRUE(prop.is_input());
    } else {
      ASSERT_FALSE(prop.is_input());
    }

    can_mute_ = prop.can_mute();
    can_agc_ = prop.can_agc();
    min_gain_ = prop.min_gain_db();
    max_gain_ = prop.max_gain_db();
    gain_step_ = prop.gain_step_db();

    ASSERT_TRUE(min_gain_ <= max_gain_);
    ASSERT_TRUE(gain_step_ >= 0);
    ASSERT_TRUE(gain_step_ <= std::abs(max_gain_ - min_gain_));

    plug_detect_capabilities_ = prop.plug_detect_capabilities();

    AUD_VLOG(TRACE) << "Received manufacturer " << prop.manufacturer();
    AUD_VLOG(TRACE) << "Received product " << prop.product();

    received_get_stream_properties_ = true;
  });
  RunLoopUntil([this]() { return received_get_stream_properties_; });
}

// Request that the driver return its gain capabilities and current state.
void AudioDriverTest::RequestGain() {
  // Since we reconnect to the audio stream every time we run this test and we are guaranteed by the
  // audio driver interface definition that the driver will reply to the first watch request, we
  // can get the gain state by issuing a watch FIDL call.
  stream_config_intf_->WatchGainState([this](fuchsia::hardware::audio::GainState gain_state) {
    AUD_VLOG(TRACE) << "Received gain " << gain_state.gain_db();

    cur_mute_ = gain_state.has_muted() && gain_state.muted();
    cur_agc_ = gain_state.has_agc_enabled() && gain_state.agc_enabled();
    cur_gain_ = gain_state.gain_db();

    EXPECT_EQ(can_mute_, gain_state.has_muted());
    EXPECT_EQ(can_agc_, gain_state.has_agc_enabled());

    if (cur_mute_) {
      EXPECT_TRUE(can_mute_);
    }
    if (cur_agc_) {
      EXPECT_TRUE(can_agc_);
    }
    EXPECT_GE(cur_gain_, min_gain_);
    EXPECT_LE(cur_gain_, max_gain_);
    if (max_gain_ > min_gain_) {
      EXPECT_GT(gain_step_, 0.0f);
    } else {
      EXPECT_EQ(gain_step_, 0.0f);
    }
    received_get_gain_ = true;
  });
  RunLoopUntil([this]() { return received_get_gain_; });
}

// Determine an appropriate gain state to request, then call other method to request to the
// driver. This method assumes that the driver has already successfully responded to a GetGain
// request.
void AudioDriverTest::RequestSetGain() {
  ASSERT_TRUE(received_get_gain_);

  if (max_gain_ == min_gain_) {
    FX_LOGS(WARNING) << "*** Audio " << ((device_type_ == DeviceType::Input) ? "input" : "output")
                     << " has fixed gain (" << cur_gain_ << " dB). Skipping SetGain test. ***";
    return;
  }

  set_gain_ = min_gain_;
  if (cur_gain_ == min_gain_) {
    set_gain_ += gain_step_;
  }

  set_mute_ = cur_mute_;
  set_agc_ = cur_agc_;
  set_gain_ = cur_gain_;

  fuchsia::hardware::audio::GainState gain_state = {};
  gain_state.set_muted(set_mute_);
  gain_state.set_agc_enabled(set_agc_);
  gain_state.set_gain_db(set_gain_);

  AUD_VLOG(TRACE) << "Sent gain " << gain_state.gain_db();
  stream_config_intf_->SetGain(std::move(gain_state));
}

// Request that the driver return the format ranges that it supports.
void AudioDriverTest::RequestFormats() {
  stream_config_intf_->GetSupportedFormats(
      [this](std::vector<fuchsia::hardware::audio::SupportedFormats> supported_formats) {
        EXPECT_GT(supported_formats.size(), 0u);

        for (size_t i = 0; i < supported_formats.size(); ++i) {
          auto& format = supported_formats[i].pcm_supported_formats();

          uint8_t largest_bytes_per_sample = 0;
          EXPECT_NE(format.bytes_per_sample.size(), 0u);
          for (size_t j = 0; j < format.bytes_per_sample.size(); ++j) {
            EXPECT_NE(format.bytes_per_sample[j], 0u);
            if (format.bytes_per_sample[j] > largest_bytes_per_sample) {
              largest_bytes_per_sample = format.bytes_per_sample[j];
            }
          }
          for (size_t j = 0; j < format.valid_bits_per_sample.size(); ++j) {
            EXPECT_LE(format.valid_bits_per_sample[j], largest_bytes_per_sample * 8);
          }

          EXPECT_NE(format.frame_rates.size(), 0u);
          for (size_t j = 0; j < format.frame_rates.size(); ++j) {
            EXPECT_GE(format.frame_rates[j], fuchsia::media::MIN_PCM_FRAMES_PER_SECOND);
            EXPECT_LE(format.frame_rates[j], fuchsia::media::MAX_PCM_FRAMES_PER_SECOND);
          }

          EXPECT_NE(format.number_of_channels.size(), 0u);
          for (size_t j = 0; j < format.number_of_channels.size(); ++j) {
            EXPECT_GE(format.number_of_channels[j], fuchsia::media::MIN_PCM_CHANNEL_COUNT);
            EXPECT_LE(format.number_of_channels[j], fuchsia::media::MAX_PCM_CHANNEL_COUNT);
          }

          pcm_formats_.push_back(format);
        }

        received_get_formats_ = true;
      });
  RunLoopUntil([this]() { return received_get_formats_; });
}

// For the channelization and sample_format that we've set, determine the size of each frame.
// This method assumes that SetFormat has already been sent to the driver.
void AudioDriverTest::CalculateFrameSize() {
  if (!format_is_set_) {
    return;
  }
  EXPECT_LE(pcm_format_.valid_bits_per_sample, pcm_format_.bytes_per_sample * 8);
  frame_size_ = pcm_format_.number_of_channels * pcm_format_.bytes_per_sample;
}

void AudioDriverTest::SelectFirstFormat() {
  if (received_get_formats_) {
    ASSERT_NE(pcm_formats_.size(), 0u);

    auto& first_format = pcm_formats_[0];
    pcm_format_.number_of_channels = first_format.number_of_channels[0];
    pcm_format_.channels_to_use_bitmask = (1 << pcm_format_.number_of_channels) - 1;  // Use all.
    pcm_format_.sample_format = first_format.sample_formats[0];
    pcm_format_.bytes_per_sample = first_format.bytes_per_sample[0];
    pcm_format_.valid_bits_per_sample = first_format.valid_bits_per_sample[0];
    pcm_format_.frame_rate = first_format.frame_rates[0];
  }
}

void AudioDriverTest::SelectLastFormat() {
  if (received_get_formats_) {
    ASSERT_NE(pcm_formats_.size(), 0u);

    auto& last_format = pcm_formats_[pcm_formats_.size() - 1];
    pcm_format_.number_of_channels =
        last_format.number_of_channels[last_format.number_of_channels.size() - 1];
    pcm_format_.channels_to_use_bitmask = (1 << pcm_format_.number_of_channels) - 1;  // Use all.
    pcm_format_.sample_format = last_format.sample_formats[last_format.sample_formats.size() - 1];
    pcm_format_.bytes_per_sample =
        last_format.bytes_per_sample[last_format.bytes_per_sample.size() - 1];
    pcm_format_.valid_bits_per_sample =
        last_format.valid_bits_per_sample[last_format.valid_bits_per_sample.size() - 1];
    pcm_format_.frame_rate = last_format.frame_rates[last_format.frame_rates.size() - 1];
  }
}

void AudioDriverTest::RequestRingBuffer() {
  fuchsia::hardware::audio::Format format = {};
  format.set_pcm_format(pcm_format_);

  fidl::InterfaceHandle<fuchsia::hardware::audio::RingBuffer> ring_buffer_handle;
  stream_config_intf_->CreateRingBuffer(std::move(format), ring_buffer_handle.NewRequest());

  zx::channel channel = ring_buffer_handle.TakeChannel();
  ring_buffer_intf_ =
      fidl::InterfaceHandle<fuchsia::hardware::audio::RingBuffer>(std::move(channel)).Bind();
  if (!stream_config_intf_.is_bound()) {
    FX_LOGS(ERROR) << "Failed to get ring buffer channel";
    FAIL();
  }
  ring_buffer_intf_.set_error_handler([](zx_status_t status) {
    FX_PLOGS(ERROR, status) << "Test failed with error: " << status;
    FAIL();
  });
  format_is_set_ = true;
  ring_buffer_ready_ = true;
}

// Request that driver set format to the lowest rate/channelization of the first range reported.
// This method assumes that the driver has already successfully responded to a GetFormats request.
void AudioDriverTest::RequestRingBufferMin() {
  ASSERT_TRUE(received_get_formats_);
  ASSERT_GT(pcm_formats_.size(), 0u);

  SelectFirstFormat();
  RequestRingBuffer();
  CalculateFrameSize();
}

// Request that driver set format to the highest rate/channelization of the final range reported.
// This method assumes that the driver has already successfully responded to a GetFormats request.
void AudioDriverTest::RequestRingBufferMax() {
  ASSERT_TRUE(received_get_formats_);
  ASSERT_GT(pcm_formats_.size(), 0u);

  SelectLastFormat();
  RequestRingBuffer();
  CalculateFrameSize();
}

// Request that driver retrieve the current plug detection state.
void AudioDriverTest::RequestPlugDetect() {
  // Since we reconnect to the audio stream every time we run this test and we are guaranteed by the
  // audio driver interface definition that the driver will reply to the first watch request, we
  // can get the plug state by issuing a watch FIDL call.
  stream_config_intf_->WatchPlugState([this](fuchsia::hardware::audio::PlugState state) {
    plugged_ = state.plugged();
    plug_state_time_ = state.plug_state_time();
    EXPECT_LT(plug_state_time_, zx::clock::get_monotonic().get());

    AUD_VLOG(TRACE) << "Plug_state_time: " << plug_state_time_;
    received_plug_detect_ = true;
  });
  RunLoopUntil([this]() { return received_plug_detect_; });
}

// Ring-buffer channel requests
//
// Request that the driver return the FIFO depth (in bytes), at the currently set format.
// This method relies on the ring buffer channel.
void AudioDriverTest::RequestRingBufferProperties() {
  ASSERT_TRUE(ring_buffer_ready_);

  ring_buffer_intf_->GetProperties([this](fuchsia::hardware::audio::RingBufferProperties prop) {
    external_delay_nsec_ = prop.external_delay();
    fifo_depth_ = prop.fifo_depth();
    clock_domain_ = prop.clock_domain();
    needs_cache_flush_or_invalidate_ = prop.needs_cache_flush_or_invalidate();
    received_get_ring_buffer_properties_ = true;
  });

  // This command can return an error, so we check for error_occurred_ as well
  RunLoopUntil([this]() { return received_get_ring_buffer_properties_; });
}

// Request that the driver return a VMO handle for the ring buffer, at the currently set format.
// This method relies on the ring buffer channel.
void AudioDriverTest::RequestBuffer(uint32_t min_ring_buffer_frames,
                                    uint32_t notifications_per_ring) {
  min_ring_buffer_frames_ = min_ring_buffer_frames;
  notifications_per_ring_ = notifications_per_ring;
  zx::vmo ring_buffer_vmo;
  ring_buffer_intf_->GetVmo(
      min_ring_buffer_frames, notifications_per_ring,
      [this, &ring_buffer_vmo](fuchsia::hardware::audio::RingBuffer_GetVmo_Result result) {
        EXPECT_GE(result.response().num_frames, min_ring_buffer_frames_);
        ring_buffer_frames_ = result.response().num_frames;
        ring_buffer_vmo = std::move(result.response().ring_buffer);
        EXPECT_TRUE(ring_buffer_vmo.is_valid());
        received_get_buffer_ = true;
      });

  RunLoopUntil([this]() { return received_get_buffer_; });

  const zx_vm_option_t option_flags = ZX_VM_PERM_READ | ZX_VM_PERM_WRITE;
  EXPECT_EQ(
      ring_buffer_.CreateAndMap(ring_buffer_frames_ * frame_size_, option_flags, nullptr,
                                &ring_buffer_vmo, ZX_RIGHT_READ | ZX_RIGHT_MAP | ZX_RIGHT_TRANSFER),
      ZX_OK);

  AUD_VLOG(TRACE) << "Mapping size: " << ring_buffer_frames_ * frame_size_;
}

// Request that the driver start the ring buffer engine, responding with the start_time.
// This method assumes that the ring buffer VMO was received in a successful GetBuffer response.
void AudioDriverTest::RequestStart() {
  ASSERT_TRUE(ring_buffer_ready_);

  auto send_time = zx::clock::get_monotonic().get();
  ring_buffer_intf_->Start([this](int64_t start_time) {
    start_time_ = start_time;
    received_start_ = true;
  });
  RunLoopUntil([this]() { return received_start_; });
  EXPECT_GT(start_time_, send_time);
  // TODO(mpuryear): validate start_time is not too far in the future (it includes FIFO delay).
}

// Request that the driver stop the ring buffer engine, including quieting position notifications.
// This method assumes that the ring buffer engine has previously been successfully started.
void AudioDriverTest::RequestStop() {
  ASSERT_TRUE(received_start_);

  ring_buffer_intf_->Stop([this]() {
    position_notification_count_ = 0;
    received_stop_ = true;
  });
  RunLoopUntil([this]() { return received_stop_; });
}

// Wait for the specified number of position notifications, or timeout at 60 seconds.
void AudioDriverTest::ExpectPositionNotifyCount(uint32_t count) {
  RunLoopUntil([this, count]() {
    ring_buffer_intf_->WatchClockRecoveryPositionInfo(
        [this](fuchsia::hardware::audio::RingBufferPositionInfo position_info) {
          EXPECT_GT(notifications_per_ring_, 0u);

          auto now = zx::clock::get_monotonic().get();
          EXPECT_LT(start_time_, now);
          EXPECT_LT(position_info.timestamp, now);

          if (position_notification_count_) {
            EXPECT_GT(position_info.timestamp, start_time_);
            EXPECT_GT(position_info.timestamp, position_info_.timestamp);
          } else {
            EXPECT_GE(position_info.timestamp, start_time_);
          }

          position_info_.timestamp = position_info.timestamp;
          position_info_.position = position_info.position;
          EXPECT_LT(position_info_.position, ring_buffer_frames_ * frame_size_);

          ++position_notification_count_;

          AUD_VLOG(TRACE) << "Position: " << position_info_.position
                          << ", notification_count: " << position_notification_count_;
        });
    return position_notification_count_ >= count;
  });

  auto timestamp_duration = position_info_.timestamp - start_time_;
  auto observed_duration = zx::clock::get_monotonic().get() - start_time_;
  ASSERT_GE(position_notification_count_, count) << "No position notifications received";

  ASSERT_NE(pcm_format_.frame_rate * notifications_per_ring_, 0u);
  auto ns_per_notification =
      (zx::sec(1) * ring_buffer_frames_) / (pcm_format_.frame_rate * notifications_per_ring_);
  auto min_allowed_time = ns_per_notification.get() * (count - 1);
  auto expected_time = ns_per_notification.get() * count;
  auto max_allowed_time = ns_per_notification.get() * (count + 2) - 1;

  AUD_VLOG(TRACE) << "Timestamp delta from min/ideal/max: " << std::setw(10)
                  << (min_allowed_time - timestamp_duration) << " : " << std::setw(10)
                  << (expected_time - timestamp_duration) << " : " << std::setw(10)
                  << (max_allowed_time - timestamp_duration);
  EXPECT_GE(timestamp_duration, min_allowed_time);
  EXPECT_LE(timestamp_duration, max_allowed_time);

  AUD_VLOG(TRACE) << "Observed delta from min/ideal/max : " << std::setw(10)
                  << (min_allowed_time - observed_duration) << " : " << std::setw(10)
                  << (expected_time - observed_duration) << " : " << std::setw(10)
                  << (max_allowed_time - observed_duration);
  EXPECT_GT(observed_duration, min_allowed_time);
}

// After waiting for one second, we should NOT have received any position notifications.
void AudioDriverTest::ExpectNoPositionNotifications() {
  RunLoopUntil([this]() {
    ring_buffer_intf_->WatchClockRecoveryPositionInfo(
        [](fuchsia::hardware::audio::RingBufferPositionInfo position_info) { FAIL(); });
    return true;
  });
  zx::nanosleep(zx::deadline_after(zx::sec(1)));
  RunLoopUntilIdle();

  EXPECT_EQ(position_notification_count_, 0u);
}

//
// Test cases that target each of the various driver commands
//

// Stream channel commands
//
// For input stream, verify a valid unique_id, manufacturer, product and gain capabilites.
TEST_F(AudioDriverTest, InputStreamProperties) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestStreamProperties();
}

// For output stream, verify a valid unique_id, manufacturer, product and gain capabilites.
TEST_F(AudioDriverTest, OutputStreamProperties) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestStreamProperties();
}

// For input stream, verify a valid get gain response is successfully received.
TEST_F(AudioDriverTest, InputGetGain) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestStreamProperties();

  RequestGain();
}

// For output stream, verify a valid get gain response is successfully received.
TEST_F(AudioDriverTest, OutputGetGain) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestStreamProperties();

  RequestGain();
}

// For input stream, verify a valid set gain response is successfully received.
TEST_F(AudioDriverTest, InputSetGain) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestStreamProperties();
  RequestGain();

  RequestSetGain();
}

// For output stream, verify a valid set gain response is successfully received.
TEST_F(AudioDriverTest, OutputSetGain) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestStreamProperties();
  RequestGain();

  RequestSetGain();
}

// For input stream, verify a valid get formats response is successfully received.
TEST_F(AudioDriverTest, InputGetFormats) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
}

// For output stream, verify a valid get formats response is successfully received.
TEST_F(AudioDriverTest, OutputGetFormats) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
}

// For input stream, verify a valid plug detect response is successfully received.
TEST_F(AudioDriverTest, InputPlugDetect) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestPlugDetect();
}

// For output stream, verify a valid plug detect response is successfully received.
TEST_F(AudioDriverTest, OutputPlugDetect) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestPlugDetect();
}

// Plug detect notifications are not testable without scriptable PLUG/UNPLUG actions on actual
// hardware drivers.

// Ring Buffer channel commands
//
// For input stream, verify a valid ring buffer properties response is successfully received.
TEST_F(AudioDriverTest, InputGetRingBufferProperties) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMin();

  RequestRingBufferProperties();
}

// For output stream, verify a valid ring buffer properties response is successfully received.
TEST_F(AudioDriverTest, OutputGetRingBufferProperties) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();

  RequestRingBufferProperties();
}

// For input stream, verify a get buffer response and ring buffer VMO is successfully received.
TEST_F(AudioDriverTest, InputGetBuffer) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();

  uint32_t frames = 48000;
  uint32_t notifs = 8;
  RequestBuffer(frames, notifs);
}

// For output stream, verify a get buffer response and ring buffer VMO is successfully received.
TEST_F(AudioDriverTest, OutputGetBuffer) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMin();

  uint32_t frames = 100;
  uint32_t notifs = 1;
  RequestBuffer(frames, notifs);
}

// For input stream, verify that a valid start response is successfully received.
TEST_F(AudioDriverTest, InputStart) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();
  RequestBuffer(100, 0);

  RequestStart();
}

// For output stream, verify that a valid start response is successfully received.
TEST_F(AudioDriverTest, OutputStart) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMin();
  RequestBuffer(32000, 0);

  RequestStart();
}

// For input stream, verify that a valid stop response is successfully received.
TEST_F(AudioDriverTest, InputStop) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();
  RequestBuffer(24000, 0);
  RequestStart();

  RequestStop();
}

// For output stream, verify that a valid stop response is successfully received.
TEST_F(AudioDriverTest, OutputStop) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMin();
  RequestBuffer(100, 0);
  RequestStart();

  RequestStop();
}

// For input stream, verify position notifications at fast rate (~180/sec) over approx 100 ms.
TEST_F(AudioDriverTest, InputPositionNotifyFast) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();
  RequestBuffer(8000, 32);
  RequestStart();

  ExpectPositionNotifyCount(16);
}

// For output stream, verify position notifications at fast rate (~180/sec) over approx 100 ms.
TEST_F(AudioDriverTest, OutputPositionNotifyFast) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();
  RequestBuffer(8000, 32);
  RequestStart();

  ExpectPositionNotifyCount(16);
}

// For input stream, verify position notifications at slow rate (2/sec) over approx 1 second.
TEST_F(AudioDriverTest, InputPositionNotifySlow) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMin();
  RequestBuffer(48000, 2);
  RequestStart();

  ExpectPositionNotifyCount(2);
}

// For output stream, verify position notifications at slow rate (2/sec) over approx 1 second.
TEST_F(AudioDriverTest, OutputPositionNotifySlow) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMin();
  RequestBuffer(48000, 2);
  RequestStart();

  ExpectPositionNotifyCount(2);
}

// For input stream, verify that no position notifications arrive if notifications_per_ring is 0.
TEST_F(AudioDriverTest, InputPositionNotifyNone) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();
  RequestBuffer(8000, 0);
  RequestStart();

  ExpectNoPositionNotifications();
}

// For output stream, verify that no position notifications arrive if notifications_per_ring is 0.
TEST_F(AudioDriverTest, OutputPositionNotifyNone) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();
  RequestBuffer(8000, 0);
  RequestStart();

  ExpectNoPositionNotifications();
}

// For input stream, verify that no position notificatons arrive after stop.
TEST_F(AudioDriverTest, InputNoPositionNotifyAfterStop) {
  if (!WaitForDevice(DeviceType::Input)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();
  RequestBuffer(8000, 32);
  RequestStart();
  ExpectPositionNotifyCount(2);
  RequestStop();

  ExpectNoPositionNotifications();
}

// For output stream, verify that no position notificatons arrive after stop.
TEST_F(AudioDriverTest, OutputNoPositionNotifyAfterStop) {
  if (!WaitForDevice(DeviceType::Output)) {
    return;
  }

  RequestFormats();
  RequestRingBufferMax();
  RequestBuffer(8000, 32);
  RequestStart();
  ExpectPositionNotifyCount(2);
  RequestStop();

  ExpectNoPositionNotifications();
}

// For input stream, verify that monotonic_time values are close to NOW, and always increasing.

}  // namespace media::audio::test
