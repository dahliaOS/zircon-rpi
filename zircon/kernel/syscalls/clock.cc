// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/clock_dispatcher.h>

#include "priv.h"

zx_status_t sys_clock_create(uint32_t options, user_out_handle* clock_out) {
  KernelHandle<ClockDispatcher> handle;
  zx_rights_t rights;

  zx_status_t result = ClockDispatcher::Create(options, &handle, &rights);
  if (result == ZX_OK)
    result = clock_out->make(ktl::move(handle), rights);

  return result;
}

zx_status_t sys_clock_read(zx_handle_t clock_handle, user_out_ptr<zx_time_t> user_now) {
  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<ClockDispatcher> clock;
  zx_status_t status = up->GetDispatcherWithRights(clock_handle, ZX_RIGHT_READ, &clock);
  if (status != ZX_OK) {
    return status;
  }

  zx_time_t now;
  status = clock->Read(&now);
  if (status != ZX_OK) {
    return status;
  }

  return user_now.copy_to_user(now);
}

zx_status_t sys_clock_get_details(zx_handle_t clock_handle,
                                  user_out_ptr<zx_clock_details_t> user_details) {
  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<ClockDispatcher> clock;
  zx_status_t status = up->GetDispatcherWithRights(clock_handle, ZX_RIGHT_READ, &clock);
  if (status != ZX_OK) {
    return status;
  }

  zx_clock_details_t details;
  status = clock->GetDetails(&details);
  if (status != ZX_OK) {
    return status;
  }

  return user_details.copy_to_user(details);
}

zx_status_t sys_clock_update(zx_handle_t clock_handle,
                             user_in_ptr<const zx_clock_update_args_t> user_args) {
  zx_clock_update_args_t args;
  zx_status_t status = user_args.copy_from_user(&args);

  if (status != ZX_OK) {
    return status;
  }

  // Before going further, perform basic sanity checks of the update arguments.
  //
  // Only the defined flags may be present in the request, and at least one of
  // them must be specified.
  if ((args.flags & ~ZX_CLOCK_UPDATE_FLAGS_ALL) || !(args.flags & ZX_CLOCK_UPDATE_FLAGS_ALL)) {
    return ZX_ERR_INVALID_ARGS;
  }

  // The PPM adjustment must be within the legal range.
  if ((args.flags & ZX_CLOCK_UPDATE_FLAG_RATE_ADJUST_VALID) &&
      ((args.rate_adjust < ZX_CLOCK_UPDATE_MIN_RATE_ADJUST) ||
       (args.rate_adjust > ZX_CLOCK_UPDATE_MAX_RATE_ADJUST))) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<ClockDispatcher> clock;
  status = up->GetDispatcherWithRights(clock_handle, ZX_RIGHT_WRITE, &clock);
  if (status != ZX_OK) {
    return status;
  }

  return clock->Update(args);
}
