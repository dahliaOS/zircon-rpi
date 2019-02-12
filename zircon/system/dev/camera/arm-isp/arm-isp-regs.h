// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <hwreg/bitfields.h>
#include <zircon/types.h>

// Power domain.
#define AO_RTI_GEN_PWR_SLEEP0 (0x3a << 2)
#define AO_RTI_GEN_PWR_ISO0 (0x3b << 2)

// Memory PD.
#define HHI_ISP_MEM_PD_REG0 (0x45 << 2)
#define HHI_ISP_MEM_PD_REG1 (0x46 << 2)

// CLK offsets.
#define HHI_CSI_PHY_CNTL0 (0xD3 << 2)
#define HHI_CSI_PHY_CNTL1 (0x114 << 2)

#define HHI_MIPI_ISP_CLK_CNTL               (0x70 << 2)

// Reset
#define RESET4_LEVEL 0x90

namespace camera {

// Product ID
#define PRODUCT_ID_DEFAULT (2658)

class ISP_PRODUCT_ID : public hwreg::RegisterBase<ISP_PRODUCT_ID, uint32_t> {
public:
    DEF_FIELD(31, 0, product_id);
    static auto Get() { return hwreg::RegisterAddr<ISP_PRODUCT_ID>(0x4L); }
};

class ISP_INTERRUPT : public hwreg::RegisterBase<ISP_INTERRUPT, uint32_t> {
public:
    DEF_BIT(0, isp_start);
    DEF_BIT(1, isp_done);
    DEF_BIT(2, ctx_management_error);
    DEF_BIT(3, broken_frame_error);
    DEF_BIT(4, metering_AF_done);
    DEF_BIT(5, metering_AEXP_done);
    DEF_BIT(6, metering_AWB_done);
    DEF_BIT(7, metering_AEXP_1024_bin_hist_done);
    DEF_BIT(8, metering_antifog_hist_done);
    DEF_BIT(9, lut_init_done);
    DEF_BIT(11, FR_y_DMA_write_done);
    DEF_BIT(12, FR_uv_DMA_write_done);
    DEF_BIT(13, DS_y_DMA_write_done);
    DEF_BIT(14, linearization_done);
    DEF_BIT(15, static_dpc_done);
    DEF_BIT(16, ca_correction_done);
    DEF_BIT(17, iridix_done);
    DEF_BIT(18, three_d_liut_done);
    DEF_BIT(19, wdg_timer_timed_out);
    DEF_BIT(20, frame_collision_error);
    DEF_BIT(21, luma_variance_done);
    DEF_BIT(22, DMA_error_interrupt);
    DEF_BIT(23, input_port_safely_stopped);
};

class ISP_MASK_VECTOR : public camera::ISP_INTERRUPT {
public:
    ISP_MASK_VECTOR& mask_all() {
        set_isp_start(1);
        set_isp_done(1);
        set_ctx_management_error(1);
        set_broken_frame_error(1);
        set_metering_AF_done(1);
        set_metering_AEXP_done(1);
        set_metering_AWB_done(1);
        set_metering_AEXP_1024_bin_hist_done(1);
        set_metering_antifog_hist_done(1);
        set_lut_init_done(1);
        set_FR_y_DMA_write_done(1);
        set_FR_uv_DMA_write_done(1);
        set_DS_y_DMA_write_done(1);
        set_linearization_done(1);
        set_static_dpc_done(1);
        set_ca_correction_done(1);
        set_iridix_done(1);
        set_three_d_liut_done(1);
        set_wdg_timer_timed_out(1);
        set_frame_collision_error(1);
        set_luma_variance_done(1);
        set_DMA_error_interrupt(1);
        set_input_port_safely_stopped(1);
        return *this;
    }

    ISP_MASK_VECTOR& unmask_all() {
        set_isp_start(0);
        set_isp_done(0);
        set_ctx_management_error(0);
        set_broken_frame_error(0);
        set_metering_AF_done(0);
        set_metering_AEXP_done(0);
        set_metering_AWB_done(0);
        set_metering_AEXP_1024_bin_hist_done(0);
        set_metering_antifog_hist_done(0);
        set_lut_init_done(0);
        set_FR_y_DMA_write_done(0);
        set_FR_uv_DMA_write_done(0);
        set_DS_y_DMA_write_done(0);
        set_linearization_done(0);
        set_static_dpc_done(0);
        set_ca_correction_done(0);
        set_iridix_done(0);
        set_three_d_liut_done(0);
        set_wdg_timer_timed_out(0);
        set_frame_collision_error(0);
        set_luma_variance_done(0);
        set_DMA_error_interrupt(0);
        set_input_port_safely_stopped(0);
        return *this;
    }
    static auto Get() { return hwreg::RegisterAddr<ISP_INTERRUPT>(0x30L); }
};

class ISP_CLEAR_VECTOR : public camera::ISP_INTERRUPT {
public:
    static auto Get() { return hwreg::RegisterAddr<ISP_INTERRUPT>(0x34L); }
};

class ISP_CLEAR : public camera::ISP_INTERRUPT {
public:
    static auto Get() { return hwreg::RegisterAddr<ISP_INTERRUPT>(0x40L); }
};

class ISP_STATUS_VECTOR : public camera::ISP_INTERRUPT {
public:
    bool has_errors() const {
        return (broken_frame_error() || frame_collision_error() ||
                DMA_error_interrupt() || ctx_management_error() ||
                wdg_timer_timed_out());
    }

    static auto Get() { return hwreg::RegisterAddr<ISP_INTERRUPT>(0x44L); }
};


} // namespace camera
