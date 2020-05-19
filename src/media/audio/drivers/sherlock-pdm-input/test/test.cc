// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/audio/llcpp/fidl.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/mock-i2c/mock-i2c.h>

#include <audio-proto/audio-proto.h>
#include <mock-mmio-reg/mock-mmio-reg.h>
#include <mock/ddktl/protocol/gpio.h>
#include <zxtest/zxtest.h>

#include "../audio-stream-in.h"

namespace audio {
namespace sherlock {

static constexpr uint32_t kTestFrameRate1 = 48000;
static constexpr uint32_t kTestFrameRate2 = 96000;
static constexpr uint8_t kTestNumberOfChannels = 2;
static constexpr uint32_t kTestFifoDepth = 16;

using ::llcpp::fuchsia::hardware::audio::Device;

audio_fidl::PcmFormat GetDefaultPcmFormat() {
  audio_fidl::PcmFormat format;
  format.number_of_channels = 2;
  format.channels_to_use_bitmask = 0x03;
  format.sample_format = audio_fidl::SampleFormat::PCM_SIGNED;
  format.frame_rate = kTestFrameRate1;
  format.bytes_per_sample = 2;
  format.valid_bits_per_sample = 16;
  return format;
}
struct TestAmlPdmDevice : public AmlPdmDevice {
  static std::unique_ptr<TestAmlPdmDevice> Create() {
    constexpr size_t n_registers = 4096;  // big enough.
    static fbl::Array<ddk_mock::MockMmioReg> unused_mocks =
        fbl::Array(new ddk_mock::MockMmioReg[n_registers], n_registers);
    static ddk_mock::MockMmioRegRegion unused_region(unused_mocks.data(), sizeof(uint32_t),
                                                     n_registers);
    return std::make_unique<TestAmlPdmDevice>(unused_region.GetMmioBuffer(),
                                              unused_region.GetMmioBuffer(), HIFI_PLL, 3, 249,
                                              TODDR_B, kTestFifoDepth, AmlVersion::kS905D2G);
  }
  TestAmlPdmDevice(ddk::MmioBuffer pdm_mmio, ddk::MmioBuffer audio_mmio,
                   ee_audio_mclk_src_t clk_src, uint32_t sysclk_div, uint32_t dclk_div,
                   aml_toddr_t toddr, uint32_t fifo_depth, AmlVersion version)
      : AmlPdmDevice(std::move(pdm_mmio), std::move(audio_mmio), clk_src, sysclk_div, dclk_div,
                     toddr, fifo_depth, version) {}
  virtual void ConfigPdmIn(uint8_t mask) { mask_ = mask; }
  uint8_t mask_;
};

struct TestStream : public SherlockAudioStreamIn {
  TestStream(zx_device_t* parent) : SherlockAudioStreamIn(parent) {
    pdm_ = TestAmlPdmDevice::Create();
  }
  zx_status_t Init() __TA_REQUIRES(domain_token()) override {
    audio_stream_format_range_t range;
    range.min_channels = kTestNumberOfChannels;
    range.max_channels = kTestNumberOfChannels;
    range.sample_formats = AUDIO_SAMPLE_FORMAT_16BIT;
    range.min_frames_per_second = kTestFrameRate1;
    range.max_frames_per_second = kTestFrameRate2;
    range.flags = ASF_RANGE_FLAG_FPS_48000_FAMILY;
    supported_formats_.push_back(range);

    fifo_depth_ = kTestFifoDepth;

    cur_gain_state_ = {};

    snprintf(device_name_, sizeof(device_name_), "test-audio-in");
    snprintf(mfr_name_, sizeof(mfr_name_), "Bike Sheds, Inc.");
    snprintf(prod_name_, sizeof(prod_name_), "testy_mctestface");

    unique_id_ = AUDIO_STREAM_UNIQUE_ID_BUILTIN_MICROPHONE;

    return ZX_OK;
  }
  uint8_t config_mask() { return static_cast<TestAmlPdmDevice*>(pdm_.get())->mask_; }
};

TEST(SherlockAudioStreamInTest, ChannelsToUseBitmask) {
  fake_ddk::Bind tester;

  auto server = audio::SimpleAudioStream::Create<TestStream>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  Device::SyncClient client_wrap(std::move(tester.FidlClient()));
  Device::ResultOf::GetChannel channel_wrap = client_wrap.GetChannel();
  ASSERT_EQ(channel_wrap.status(), ZX_OK);

  audio_fidl::StreamConfig::SyncClient client(std::move(channel_wrap->channel));


  // Test that default channels_to_use_bitmask = 0x0000'0000'0000'0003 results in a 0x03 mask.
  {
    audio_fidl::PcmFormat pcm_format = GetDefaultPcmFormat();
    fidl::aligned<audio_fidl::PcmFormat> aligned_pcm_format = std::move(pcm_format);
    auto builder = audio_fidl::Format::UnownedBuilder();
    builder.set_pcm_format(fidl::unowned_ptr(&aligned_pcm_format));
    zx::channel local, remote;
    ASSERT_OK(zx::channel::create(0, &local, &remote));
    client.CreateRingBuffer(builder.build(), std::move(remote));
    // To make sure we have initialized in the server make a sync call
    // (we know the server is single threaded, initialization is completed if received a reply).
    auto props = audio_fidl::RingBuffer::Call::GetProperties(zx::unowned_channel(local));
    ASSERT_OK(props.status());
    EXPECT_EQ(0x3, server->config_mask());
  }

  // Test that default channels_to_use_bitmask = 0x0000'0000'0000'FFFF results in a 0x03 mask.
  {
    audio_fidl::PcmFormat pcm_format = GetDefaultPcmFormat();
    pcm_format.channels_to_use_bitmask = 0xffff;
    fidl::aligned<audio_fidl::PcmFormat> aligned_pcm_format = std::move(pcm_format);
    auto builder = audio_fidl::Format::UnownedBuilder();
    builder.set_pcm_format(fidl::unowned_ptr(&aligned_pcm_format));
    zx::channel local, remote;
    ASSERT_OK(zx::channel::create(0, &local, &remote));
    client.CreateRingBuffer(builder.build(), std::move(remote));
    // To make sure we have initialized in the server make a sync call
    // (we know the server is single threaded, initialization is completed if received a reply).
    auto props = audio_fidl::RingBuffer::Call::GetProperties(zx::unowned_channel(local));
    ASSERT_OK(props.status());
    EXPECT_EQ(0x3, server->config_mask());
  }

  // Test that default channels_to_use_bitmask = 0x0000'0000'0000'0002 results in a 0x02 mask.
  {
    audio_fidl::PcmFormat pcm_format = GetDefaultPcmFormat();
    pcm_format.channels_to_use_bitmask = 0x0000'0000'0000'0002;
    fidl::aligned<audio_fidl::PcmFormat> aligned_pcm_format = std::move(pcm_format);
    auto builder = audio_fidl::Format::UnownedBuilder();
    builder.set_pcm_format(fidl::unowned_ptr(&aligned_pcm_format));
    zx::channel local, remote;
    ASSERT_OK(zx::channel::create(0, &local, &remote));
    client.CreateRingBuffer(builder.build(), std::move(remote));
    // To make sure we have initialized in the server make a sync call
    // (we know the server is single threaded, initialization is completed if received a reply).
    auto props = audio_fidl::RingBuffer::Call::GetProperties(zx::unowned_channel(local));
    ASSERT_OK(props.status());
    EXPECT_EQ(0x2, server->config_mask());
  }

  server->DdkUnbindDeprecated();
  EXPECT_TRUE(tester.Ok());
  server->DdkRelease();
}

}  // namespace sherlock
}  // namespace audio
