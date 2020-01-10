// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/clock.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <algorithm>
#include <limits>
#include <memory>

#include <audio-utils/audio-input.h>
#include <audio-utils/audio-stream.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>

namespace audio {
namespace utils {

static constexpr zx_duration_t CHUNK_TIME = ZX_MSEC(100);
static constexpr float MIN_DURATION = 0.100f;
static constexpr float MAX_DURATION = 86400.0f;

std::unique_ptr<AudioInput> AudioInput::Create(uint32_t dev_id) {
  fbl::AllocChecker ac;
  std::unique_ptr<AudioInput> res(new (&ac) AudioInput(dev_id));
  if (!ac.check())
    return nullptr;
  return res;
}

std::unique_ptr<AudioInput> AudioInput::Create(const char* dev_path) {
  fbl::AllocChecker ac;
  std::unique_ptr<AudioInput> res(new (&ac) AudioInput(dev_path));
  if (!ac.check())
    return nullptr;
  return res;
}

zx_status_t AudioInput::Record(AudioSink& sink, float duration_seconds) {
  AudioStream::Format fmt = {
      .frame_rate = frame_rate_,
      .channels = static_cast<uint16_t>(channel_cnt_),
      .sample_format = sample_format_,
  };

  duration_seconds = fbl::clamp(duration_seconds, MIN_DURATION, MAX_DURATION);

  zx_status_t res = sink.SetFormat(fmt);
  if (res != ZX_OK) {
    printf("Failed to set sink format (rate %u, chan_count %u, fmt 0x%08x, res %d)\n", frame_rate_,
           channel_cnt_, sample_format_, res);
    return res;
  }

  uint64_t ring_bytes_64 = (zx_duration_mul_int64(CHUNK_TIME, frame_rate_) / ZX_SEC(1)) * frame_sz_;
  if (ring_bytes_64 > std::numeric_limits<uint32_t>::max()) {
    printf("Invalid frame rate %u\n", frame_rate_);
    return res;
  }

  uint32_t ring_bytes = static_cast<uint32_t>(ring_bytes_64);
  uint32_t ring_frames = ring_bytes / frame_sz_;

  res = GetBuffer(ring_frames, 8u);
  if (res != ZX_OK) {
    printf("Failed to establish ring buffer (%u frames, res %d)\n", ring_frames, res);
    return res;
  }
  printf("Recording for %.1f seconds\n", duration_seconds);

  res = StartRingBuffer();
  if (res != ZX_OK) {
    printf("Failed to start capture (res %d)\n", res);
    return res;
  }

  auto start_time = zx::clock::get_monotonic();

  uint32_t rd_ptr = 0;    // Our read ptr for the ring buffer.
  uint32_t wr_ptr = 0;    // Estimated write ptr in the ring buffer.
  uint32_t consumed = 0;  // Total bytes consumed.
  uint32_t produced = 0;  // Estimated total bytes produced.
  uint32_t total = frame_sz_ * frame_rate_ * static_cast<uint32_t>(10.f * duration_seconds) / 10;

  // We want to process about 2 FIFOs worth of samples in each loop, we wait until we should have
  // received about 4 FIFOs before reading to make sure we are behind the HW.
  constexpr uint32_t floor_frames = 2000;
  uint32_t wait_frames = std::max(4 * fifo_depth_, floor_frames);
  uint64_t wait_usecs = (1'000'000 * static_cast<uint64_t>(wait_frames)) / frame_rate_;

  while (consumed < total) {
    uint32_t msecs = static_cast<uint32_t>((zx::clock::get_monotonic() - start_time).to_msecs());
    uint32_t current = msecs * frame_rate_ / 1'000 * frame_sz_;

    if (current - consumed < wait_frames * frame_sz_) {
      zx::nanosleep(zx::deadline_after(zx::usec(wait_usecs)));
    }
    msecs = static_cast<uint32_t>((zx::clock::get_monotonic() - start_time).to_msecs());
    current = msecs * frame_rate_ / 1'000 * frame_sz_;

    uint32_t increment = current - produced;
    ZX_DEBUG_ASSERT(increment > 2 * fifo_depth_);
    increment -= 2 * fifo_depth_;
    consumed += increment;
    if (consumed > total) {
      increment = total - (consumed - increment);
    }

    wr_ptr += increment;
    produced += increment;
    if (wr_ptr > rb_sz_) {
      wr_ptr -= rb_sz_;
    }

    uint32_t todo = wr_ptr + rb_sz_ - rd_ptr;
    if (todo >= rb_sz_) {
      todo -= rb_sz_;
    }

    ZX_DEBUG_ASSERT(todo < rb_sz_);
    ZX_DEBUG_ASSERT(rd_ptr < rb_sz_);

    uint32_t space = rb_sz_ - rd_ptr;
    uint32_t amt = std::min(space, todo);
    auto data = static_cast<const uint8_t*>(rb_virt_) + rd_ptr;

    res = zx_cache_flush(data, amt, ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE);
    if (res != ZX_OK) {
      printf("Failed to cache invalidate(res %d).\n", res);
      break;
    }

    res = sink.PutFrames(data, amt);
    if (res != ZX_OK) {
      printf("Failed to record %u bytes (res %d)\n", amt, res);
      break;
    }

    if (amt < todo) {
      amt = todo - amt;
      ZX_DEBUG_ASSERT(amt < rb_sz_);

      res = zx_cache_flush(rb_virt_, amt, ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE);
      if (res != ZX_OK) {
        printf("Failed to cache invalidate(res %d) %d\n", res, __LINE__);
        break;
      }

      res = sink.PutFrames(rb_virt_, amt);
      if (res != ZX_OK) {
        printf("Failed to record %u bytes (res %d)\n", amt, res);
        break;
      }

      rd_ptr = amt;
    } else {
      rd_ptr += amt;
      if (rd_ptr >= rb_sz_) {
        ZX_DEBUG_ASSERT(rd_ptr == rb_sz_);
        rd_ptr = 0;
      }
    }
  }

  StopRingBuffer();

  zx_status_t finalize_res = sink.Finalize();
  return (res == ZX_OK) ? finalize_res : res;
}

}  // namespace utils
}  // namespace audio
