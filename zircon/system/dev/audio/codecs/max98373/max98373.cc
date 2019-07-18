// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "max98373.h"

#include <algorithm>
#include <memory>

#include <ddk/binding.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <ddk/protocol/i2c.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <zircon/time.h>

namespace {

// clang-format off
constexpr uint16_t kRegReset                   = 0x2000;
constexpr uint16_t kRegGlobalEnable            = 0x20ff;
constexpr uint16_t kRegPcmInterfaceFormat      = 0x2024;
constexpr uint16_t kRegPcmInterfaceClkRatio    = 0x2026;
constexpr uint16_t kRegPcmInterfaceSampleRate  = 0x2027;
constexpr uint16_t kRegPcmInterfaceInput       = 0x202b;
constexpr uint16_t kRegDigitalVol              = 0x203d;
constexpr uint16_t kRegSpkPathAndDspEnable     = 0x2043;
constexpr uint16_t kRegRevId                   = 0x21ff;

constexpr uint8_t kRegSpkPathAndDspEnableSpkOn = 0x01;
constexpr uint8_t kRegGlobalEnableOn           = 0x01;
constexpr uint8_t kRegResetReset               = 0x01;
// clang-format on

// TODO(andresoportus): Add handling for the other formats supported by this codec.
static const uint32_t supported_n_channels[] = {2};
static const sample_format_t supported_sample_formats[] = {SAMPLE_FORMAT_PCM_SIGNED};
static const justify_format_t supported_justify_formats[] = {JUSTIFY_FORMAT_JUSTIFY_I2S};
static const uint32_t supported_rates[] = {48000};
static const uint8_t supported_bits_per_channel[] = {32};
static const uint8_t supported_bits_per_sample[] = {32};
static const dai_supported_formats_t kSupportedDaiFormats = {
    .number_of_channels_list = supported_n_channels,
    .number_of_channels_count = countof(supported_n_channels),
    .sample_formats_list = supported_sample_formats,
    .sample_formats_count = countof(supported_sample_formats),
    .justify_formats_list = supported_justify_formats,
    .justify_formats_count = countof(supported_justify_formats),
    .frame_rates_list = supported_rates,
    .frame_rates_count = countof(supported_rates),
    .bits_per_channel_list = supported_bits_per_channel,
    .bits_per_channel_count = countof(supported_bits_per_channel),
    .bits_per_sample_list = supported_bits_per_sample,
    .bits_per_sample_count = countof(supported_bits_per_sample),
};

}  // namespace

namespace audio {

int Max98373::Thread() {
  auto status = SoftwareResetAndInitialize();
  if (status != ZX_OK) {
    return thrd_error;
  }
  return thrd_success;
}

zx_status_t Max98373::SoftwareResetAndInitialize() {
  fbl::AutoLock lock(&lock_);
  auto status = WriteReg(kRegReset, kRegResetReset);
  if (status != ZX_OK) {
    return status;
  }
  zx_nanosleep(zx_deadline_after(ZX_MSEC(50)));

  uint8_t buffer;
  status = ReadReg(kRegRevId, &buffer);
  if (status == ZX_OK && buffer != 0x43) {
    zxlogf(ERROR, "%s Unexpected Rev Id 0x%02X\n", __FILE__, buffer);
    return ZX_ERR_INTERNAL;
  }

  constexpr float initial_gain = -10.f;
  constexpr struct {
    uint16_t reg;
    uint8_t value;
  } kDefaults[] = {
      {kRegGlobalEnable, kRegGlobalEnableOn},
      {kRegSpkPathAndDspEnable, kRegSpkPathAndDspEnableSpkOn},
      {kRegDigitalVol, static_cast<uint8_t>(-initial_gain * 2.f)},
      {kRegPcmInterfaceInput, 0x01},       // PCM DIN enable.
      {kRegPcmInterfaceFormat, 0xc0},      // I2S 32 bits. LRCLK starts low.
      {kRegPcmInterfaceClkRatio, 0x4},      // 64 BCLKs per LRCLK
      {kRegPcmInterfaceSampleRate, 0x08},  // 48KHz.
  };
  for (auto& i : kDefaults) {
    auto status = WriteReg(i.reg, i.value);
    if (status != ZX_OK) {
      return status;
    }
  }

  initialized_ = true;
  zxlogf(INFO, "audio: codec max98373 initialized\n");
  TestLocked();
  return status;
}

void Max98373::TestLocked() {
  struct Command {
    uint16_t reg;
    uint8_t value;
  };
  constexpr Command kSequence1[] = {
      {0x2025, 0x00}, // PCM Clock Cfg: PCM slave interface mode
      {0x2040, 0x6}, // Tone gen: fs/64 @ 48KHz = 750Hz
      {0x2043, 0x1}, // SPK enable
  };
  for (auto& i : kSequence1) {
    auto status = WriteReg(i.reg, i.value);
    if (status != ZX_OK) {
      zxlogf(WARN, "Failed to set register 0x%x to 0x%x: %d\n", i.reg, i.value, status);
    }
  }

  zx_nanosleep(zx_deadline_after(ZX_SEC(2)));

  constexpr Command kSequence2[] = {
      {0x2043, 0x0}, // SPK disable
      {0x2040, 0x0}, // Tone gen disable
  };
  for (auto& i : kSequence2) {
    auto status = WriteReg(i.reg, i.value);
    if (status != ZX_OK) {
      zxlogf(WARN, "Failed to set register 0x%x to 0x%x: %d\n", i.reg, i.value, status);
    }
  }
}

zx_status_t Max98373::Bind() {
  auto thunk = [](void* arg) -> int { return reinterpret_cast<Max98373*>(arg)->Thread(); };
  int rc = thrd_create_with_name(&thread_, thunk, this, "Max98373-thread");
  if (rc != thrd_success) {
    return ZX_ERR_INTERNAL;
  }
  zx_device_prop_t props[] = {
      {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_MAXIM},
      {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_MAXIM_MAX98373},
  };
  return DdkAdd("max98373", 0, props, countof(props));
}

void Max98373::Shutdown() { thrd_join(thread_, NULL); }

zx_status_t Max98373::Create(zx_device_t* parent) {
  auto dev = std::make_unique<Max98373>(parent, ddk::I2cChannel(parent));
  zx_status_t st = dev->Bind();
  if (st == ZX_OK) {
    // devmgr is now in charge of the memory for dev
    __UNUSED auto ptr = dev.release();
    return st;
  }
  return ZX_OK;
}

void Max98373::CodecReset(codec_reset_callback callback, void* cookie) {
  auto status = SoftwareResetAndInitialize();
  callback(cookie, status);
}

void Max98373::CodecGetInfo(codec_get_info_callback callback, void* cookie) {
  info_t info;
  info.unique_id = "";
  info.manufacturer = "Maxim";
  info.product_name = "MAX98373";
  callback(cookie, &info);
}

void Max98373::CodecIsBridgeable(codec_is_bridgeable_callback callback, void* cookie) {
  callback(cookie, false);
}

void Max98373::CodecSetBridgedMode(bool enable_bridged_mode,
                                   codec_set_bridged_mode_callback callback, void* cookie) {
  // TODO(andresoportus): Add support and report true in CodecIsBridgeable.
  callback(cookie);
}

void Max98373::CodecGetDaiFormats(codec_get_dai_formats_callback callback, void* cookie) {
  callback(cookie, ZX_OK, &kSupportedDaiFormats, 1);
}

void Max98373::CodecSetDaiFormat(const dai_format_t* format, codec_set_dai_format_callback callback,
                                 void* cookie) {
  if (format == nullptr) {
    callback(cookie, ZX_ERR_INVALID_ARGS);
    return;
  }

  // Only allow 2 channels.
  if (format->number_of_channels != 2) {
    zxlogf(ERROR, "%s DAI format number of channels not supported\n", __FILE__);
    callback(cookie, ZX_ERR_NOT_SUPPORTED);
    return;
  }
  if (format->channels_to_use_count != 2 || format->channels_to_use_list == nullptr ||
      format->channels_to_use_list[0] != 0 || format->channels_to_use_list[1] != 1) {
    zxlogf(ERROR, "%s DAI format channels to use not supported\n", __FILE__);
    callback(cookie, ZX_ERR_NOT_SUPPORTED);
    return;
  }

  // Only I2S.
  if (format->sample_format != SAMPLE_FORMAT_PCM_SIGNED ||
      format->justify_format != JUSTIFY_FORMAT_JUSTIFY_I2S) {
    zxlogf(ERROR, "%s DAI format format not supported\n", __FILE__);
    callback(cookie, ZX_ERR_NOT_SUPPORTED);
    return;
  }

  // Check rates allowed.
  size_t i = 0;
  for (i = 0; i < kSupportedDaiFormats.frame_rates_count; ++i) {
    if (format->frame_rate == kSupportedDaiFormats.frame_rates_list[i]) {
      break;
    }
  }
  if (i == kSupportedDaiFormats.frame_rates_count) {
    zxlogf(ERROR, "%s DAI format rates not supported\n", __FILE__);
    callback(cookie, ZX_ERR_NOT_SUPPORTED);
    return;
  }

  // Allow only 32 bits samples and channel.
  if (format->bits_per_sample != 32 || format->bits_per_channel != 32) {
    callback(cookie, ZX_ERR_NOT_SUPPORTED);
    return;
  }
  callback(cookie, ZX_OK);
}

void Max98373::CodecGetGainFormat(codec_get_gain_format_callback callback, void* cookie) {
  gain_format_t format = {};
  format.type = GAIN_TYPE_DECIBELS;
  format.min_gain = kMinGain;
  format.max_gain = kMaxGain;
  format.gain_step = kGainStep;
  format.can_mute = false;
  format.can_agc = false;
  callback(cookie, &format);
}

void Max98373::CodecSetGainState(const gain_state_t* gain_state,
                                 codec_set_gain_state_callback callback, void* cookie) {
  fbl::AutoLock lock(&lock_);
  float gain = std::clamp(gain_state->gain, kMinGain, kMaxGain);
  uint8_t gain_reg = static_cast<uint8_t>(-gain * 2.f);
  zx_status_t status = WriteReg(kRegDigitalVol, gain_reg);
  if (status != ZX_OK) {
    callback(cookie);
    return;
  }
  current_gain_ = gain;
  callback(cookie);
}

void Max98373::CodecGetGainState(codec_get_gain_state_callback callback, void* cookie) {
  gain_state_t gain_state = {};
  gain_state.gain = current_gain_;
  gain_state.muted = false;
  gain_state.agc_enable = false;
  callback(cookie, &gain_state);
}

void Max98373::CodecGetPlugState(codec_get_plug_state_callback callback, void* cookie) {
  callback(cookie, nullptr);
}

zx_status_t Max98373::WriteReg(uint16_t reg, uint8_t value) {
  uint8_t write_buffer[3];
  write_buffer[0] = static_cast<uint8_t>((reg >> 8) & 0xff);
  write_buffer[1] = static_cast<uint8_t>((reg >> 0) & 0xff);
  write_buffer[2] = value;
#define TRACE_I2C
#ifdef TRACE_I2C
  printf("%s Writing register 0x%02X to value 0x%02X\n", __FILE__, reg, value);
  auto status = i2c_.WriteSync(write_buffer, countof(write_buffer));
  if (status != ZX_OK) {
    printf("%s Could not I2C write %d\n", __FILE__, status);
    return status;
  }
  uint8_t buffer = 0;
  i2c_.WriteReadSync(write_buffer, countof(write_buffer) - 1, &buffer, 1);
  if (status != ZX_OK) {
    printf("%s Could not I2C read %d\n", __FILE__, status);
    return status;
  }
  printf("%s Read register just written 0x%04X, value 0x%02X\n", __FILE__, reg, buffer);
  return ZX_OK;
#else
  return i2c_.WriteSync(write_buffer, countof(write_buffer));
#endif
}

zx_status_t Max98373::ReadReg(uint16_t reg, uint8_t* value) {
  uint8_t write_buffer[2];
  write_buffer[0] = static_cast<uint8_t>((reg >> 8) & 0xff);
  write_buffer[1] = static_cast<uint8_t>((reg >> 0) & 0xff);
  auto status = i2c_.WriteReadSync(write_buffer, 2, value, 1);
  if (status != ZX_OK) {
    printf("%s Could not I2C read reg 0x%X status %d\n", __FILE__, reg, status);
    return status;
  }
//#define TRACE_I2C
#ifdef TRACE_I2C
  printf("%s Read register 0x%04X, value 0x%02X\n", __FILE__, reg, *value);
#endif
  return status;
}

zx_status_t max98373_bind(void* ctx, zx_device_t* parent) { return Max98373::Create(parent); }

static zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops;
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = max98373_bind;
  return ops;
}();

}  // namespace audio

// clang-format off
ZIRCON_DRIVER_BEGIN(max98373, audio::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
    BI_ABORT_IF(NE, BIND_ACPI_HID_0_3, 0x4d583938), // 'MX98'
    BI_MATCH_IF(EQ, BIND_ACPI_HID_4_7, 0x33373300), // '373\0'
ZIRCON_DRIVER_END(max98373)
// clang-format on
/*
// clang-format off
ZIRCON_DRIVER_BEGIN(ti_max98373, audio::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_MAXIM),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_MAXIM_MAX98373),
ZIRCON_DRIVER_END(ti_max98373)
*/
