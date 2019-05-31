// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <acpica/acpi.h>

ACPI_STATUS acpi_internal_init(bool use_hardware_iommu, const char* board_name,
                               size_t board_name_size);
