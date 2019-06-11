// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <err.h>
#include <lib/affine/ratio.h>
#include <lib/affine/transform.h>
#include <lib/counters.h>
#include <zircon/rights.h>
#include <zircon/syscalls/clock.h>

#include <fbl/alloc_checker.h>
#include <object/clock_dispatcher.h>

KCOUNTER(dispatcher_clock_create_count, "dispatcher.clock.create")
KCOUNTER(dispatcher_clock_destroy_count, "dispatcher.clock.destroy")

namespace {

inline zx_clock_transformation_t CopyTransform(const affine::Transform& src) {
  return {src.a_offset(), src.b_offset(), {src.numerator(), src.denominator()}};
}

}  // namespace

zx_status_t ClockDispatcher::Create(uint32_t options, KernelHandle<ClockDispatcher>* handle,
                                    zx_rights_t* rights) {
  constexpr uint32_t ALL_OPTIONS = ZX_CLOCK_OPT_MONOTONIC | ZX_CLOCK_OPT_CONTINUOUS;

  // Reject any request which includes an options flag we do not recognize.
  if (~ALL_OPTIONS & options) {
    return ZX_ERR_INVALID_ARGS;
  }

  // If the user asks for a continuous clock, it must also be monotonic
  if ((options & ZX_CLOCK_OPT_CONTINUOUS) && !(options & ZX_CLOCK_OPT_MONOTONIC)) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AllocChecker ac;
  KernelHandle clock(fbl::AdoptRef(new (&ac) ClockDispatcher(options)));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  *rights = default_rights();
  *handle = ktl::move(clock);
  return ZX_OK;
}

ClockDispatcher::ClockDispatcher(uint32_t options) : options_(options) {
  kcounter_add(dispatcher_clock_create_count, 1);
}

ClockDispatcher::~ClockDispatcher() { kcounter_add(dispatcher_clock_destroy_count, 1); }

zx_status_t ClockDispatcher::Read(zx_time_t* out_now) {
  int64_t now_ticks;
  affine::Transform ticks_to_synthetic;

  while (true) {
    // load the generation counter.  If it is odd, we are in the middle of
    // an update and need to wait.  Just spin; the update operation (once
    // started) is non-preemptable and will be done very shortly.
    auto gen = gen_counter_.load(ktl::memory_order_acquire);
    if (gen & 0x1) {
      continue;
    }

    // Latch the transformation and observe the tick counter.
    ticks_to_synthetic = ticks_to_synthetic_;
    now_ticks = current_ticks();

    // If the generation counter has not changed, then we are done.
    // Otherwise, we need to start over.
    if (gen == gen_counter_.load(ktl::memory_order_acquire)) {
      break;
    }
  }

  // Perform the calculation using our latched data and we are done.
  *out_now = ticks_to_synthetic_.Apply(now_ticks);

  return ZX_OK;
}

zx_status_t ClockDispatcher::GetDetails(zx_clock_details_t* out_details) {
  while (true) {
    // load the generation counter.  If it is odd, we are in the middle of
    // an update and need to wait.  Just spin; the update operation (once
    // started) is non-preemptable and will be done very shortly.
    auto gen = gen_counter_.load(ktl::memory_order_acquire);
    if (gen & 0x1) {
      continue;
    }

    // Latch the detailed information.
    out_details->generation_counter = gen;
    out_details->ticks_to_synthetic = CopyTransform(ticks_to_synthetic_);
    out_details->mono_to_synthetic = CopyTransform(mono_to_synthetic_);
    out_details->error_bound = error_bound_;
    out_details->query_ticks = current_ticks();
    out_details->options = options_;
    out_details->last_value_update_ticks = last_value_update_ticks_;
    out_details->last_rate_adjust_update_ticks = last_rate_adjust_update_ticks_;
    out_details->last_error_bounds_update_ticks = last_error_bounds_update_ticks_;

    // If the generation counter has not changed, then we are done.
    // Otherwise, we need to start over.
    if (gen == gen_counter_.load(ktl::memory_order_acquire)) {
      break;
    }
  }

  return ZX_OK;
}

zx_status_t ClockDispatcher::Update(const zx_clock_update_args_t& args) {
  bool do_set = args.flags & ZX_CLOCK_UPDATE_FLAG_VALUE_VALID;
  bool do_rate = args.flags & ZX_CLOCK_UPDATE_FLAG_RATE_ADJUST_VALID;

  // Enter the writer lock.  Only one update can take place at a time.
  Guard<Mutex> writer_lock{&writer_lock_};

  // If the clock has not yet been defined, then we require the first update
  // to include a set operation.
  if (!do_set && !is_defined()) {
    return ZX_ERR_BAD_STATE;
  }

  // Continue with the argument sanity checking.  Set operations are not
  // allowed on continuous clocks after the very first one (which is what
  // defines the clock).
  if (do_set && is_continuous() && is_defined()) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Bump the generation counter.  This will disable all read operations until
  // we bump the counter again.  Disable rescheduling during this update.
  // This will not take very long, and it would be really bad to leave the
  // clock in a non-readable state while we are updating it (because all of
  // the readers would be blindly spinning while waiting for the clock to
  // become readable).
  AutoReschedDisable disable_resched;

  // TODO(johngro): get the barriers right here.
  auto prev_counter = gen_counter_.fetch_add(1);
  ZX_DEBUG_ASSERT((prev_counter & 0x1) == 0);
  int64_t now_ticks = static_cast<int64_t>(current_ticks());

  // Are we updating the transformations at all?
  if (do_set || do_rate) {
    int64_t now_synthetic;

    // Figure out the new synthetic offset
    if (do_set) {
      // We are performing a set operation.  If this clock is defined and
      // monotonic, and the set operation would result in non-monotonic
      // behavior for the clock, disallow it.
      if (is_defined() && is_monotonic()) {
        int64_t now_clock = ticks_to_synthetic_.Apply(now_ticks);
        if (args.value < now_clock) {
          // turns out we are not going to make any changes to the clock.
          // Put the generation counter back to where it was.
          //
          // TODO(johngro): get the barriers right here.
          gen_counter_.store(prev_counter);
          return ZX_ERR_INVALID_ARGS;
        }
      }

      // Because this is a set operation, now on the synthetic timeline is
      // what the user has specified.
      now_synthetic = args.value;
      last_value_update_ticks_ = now_ticks;
    } else {
      // Looks like we are updating the rate, but not setting the clock.  Make
      // sure that the update is 1st order continuous adjustment.
      now_synthetic = ticks_to_synthetic_.Apply(now_ticks);
    }

    // Figure out the new rates.
    affine::Ratio ticks_to_mono_ratio = platform_get_ticks_to_time_ratio();
    affine::Ratio mono_to_synthetic_rate;
    affine::Ratio ticks_to_synthetic_rate;

    if (do_rate) {
      // We want to explicitly update the rate.  Encode the PPM adjustment
      // as a ratio, then compute the ticks_to_synthetic_rate.
      mono_to_synthetic_rate = {static_cast<uint32_t>(1000000 + args.rate_adjust), 1000000};
      ticks_to_synthetic_rate = ticks_to_mono_ratio * mono_to_synthetic_rate;
      last_rate_adjust_update_ticks_ = now_ticks;
    } else if (!is_defined()) {
      // The clock has never been defined, then the default rate is 1:1
      // with the mono reference.
      mono_to_synthetic_rate = {1, 1};
      ticks_to_synthetic_rate = ticks_to_mono_ratio;
      last_rate_adjust_update_ticks_ = now_ticks;
    } else {
      // Otherwise, preserve the existing rate.
      mono_to_synthetic_rate = mono_to_synthetic_.ratio();
      ticks_to_synthetic_rate = ticks_to_synthetic_.ratio();
    }

    // Now, simply update the transformations with the proper offsets and
    // the calculated rates.
    zx_time_t now_mono = ticks_to_mono_ratio.Scale(now_ticks);
    mono_to_synthetic_ = {now_mono, now_synthetic, mono_to_synthetic_rate};
    ticks_to_synthetic_ = {now_ticks, now_synthetic, ticks_to_synthetic_rate};
  }

  // If we are supposed to update the error bound, do so.
  if (args.flags & ZX_CLOCK_UPDATE_FLAG_ERROR_BOUND_VALID) {
    error_bound_ = args.error_bound;
    last_error_bounds_update_ticks_ = now_ticks;
  }

  // We are finished.  Update the generation counter to allow clock reading again.
  // TODO(johngro): get the barriers right here.
  gen_counter_.store(prev_counter + 2);

  return ZX_OK;
}
