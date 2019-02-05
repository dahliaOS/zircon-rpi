// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/platform-defs.h>

#include "hikey960.h"

const pbus_dev_t hikey_pcie_dev = {
	.name = "hikey-pcie",

	.vid = PDEV_VID_96BOARDS,
	.pid = PDEV_PID_GENERIC,
	.did = PDEV_DID_DW_PCIE,
};

zx_status_t hikey960_pcie_init(hikey960_t* hikey) {
	zx_status_t st;

	st = pbus_device_add(&hikey->pbus, &hikey_pcie_dev);
	if (st != ZX_OK) {
		zxlogf(ERROR, "hikey960_add_device could not add hikey_usb_dev: %d\n",
			   st);
		return st;
	}

	return ZX_OK;
}