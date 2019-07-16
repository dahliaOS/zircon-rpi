// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "priv.h"

zx_status_t sys_clock_create(uint32_t options, user_out_handle* out) {
  if (options) {
    return ZX_ERR_INVALID_ARGS;
  }

  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t sys_clock_read(zx_handle_t handle, user_out_ptr<zx_time_t> now) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t sys_clock_get_details(zx_handle_t handle, user_out_ptr<zx_clock_details_t> details) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t sys_clock_update(zx_handle_t handle, user_in_ptr<const zx_clock_update_args_t> args) {
  return ZX_ERR_NOT_SUPPORTED;
}
