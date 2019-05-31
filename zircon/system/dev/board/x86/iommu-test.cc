// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zxtest/zxtest.h>
#include "iommu.h"

TEST(IommuTestCase, EnableControlEnvFlag) {
    const char board_name[] = "fake board name";

    // By default, we should use the IOMMU
    ASSERT_NULL(getenv("iommu.enable"));
    EXPECT_TRUE(iommu_use_hardware_if_present(board_name, sizeof(board_name)));

    for (auto value : {"0", "false", "off"}) {
        setenv("iommu.enable", value, true);
        EXPECT_FALSE(iommu_use_hardware_if_present(board_name, sizeof(board_name)));
    }

    for (auto value : {"1", "true", "on"}) {
        setenv("iommu.enable", value, true);
        EXPECT_TRUE(iommu_use_hardware_if_present(board_name, sizeof(board_name)));
    }
    unsetenv("iommu.enable");
}


TEST(IommuTestCase, EnableControlBuggyDevice) {
    // This device name is marked as buggy.
    const char board_name[] = "NUC6i3SYB";

    // By default, we respect the list of buggy devices
    ASSERT_NULL(getenv("iommu.enable"));
    EXPECT_FALSE(iommu_use_hardware_if_present(board_name, sizeof(board_name)));

    // If a flag setting is given, use that as an override
    for (auto value : {"0", "false", "off"}) {
        setenv("iommu.enable", value, true);
        EXPECT_FALSE(iommu_use_hardware_if_present(board_name, sizeof(board_name)));
    }

    for (auto value : {"1", "true", "on"}) {
        setenv("iommu.enable", value, true);
        EXPECT_TRUE(iommu_use_hardware_if_present(board_name, sizeof(board_name)));
    }
    unsetenv("iommu.enable");
}
