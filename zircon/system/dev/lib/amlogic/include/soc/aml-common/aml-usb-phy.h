// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#ifdef __cplusplus

#include <hwreg/bitfields.h>

class U2P_R0 : public hwreg::RegisterBase<U2P_R0, uint32_t> {
public:
    DEF_BIT(0, bypass_sel);
    DEF_BIT(1, bypass_dm_en);
    DEF_BIT(2, bypass_dp_en);
    DEF_BIT(3, txbitstuffenh);
    DEF_BIT(4, txbitstuffen);
    DEF_BIT(5, dmpulldown);
    DEF_BIT(6, dppulldown);
    DEF_BIT(7, vbusvldextsel);
    DEF_BIT(8, vbusvldext);
    DEF_BIT(9, adp_prb_en);
    DEF_BIT(10, adp_dischrg);
    DEF_BIT(11, adp_chrg);
    DEF_BIT(12, drvvbus);
    DEF_BIT(13, idpullup);
    DEF_BIT(14, loopbackenb);
    DEF_BIT(15, otgdisable);
    DEF_BIT(16, commononn);
    DEF_FIELD(19, 17, fsel);
    DEF_FIELD(21, 20, refclksel);
    DEF_BIT(22, por);
    DEF_FIELD(24, 23, vatestenb);
    DEF_BIT(25, set_iddq);
    DEF_BIT(26, ate_reset);
    DEF_BIT(27, fsv_minus);
    DEF_BIT(28, fsv_plus);
    DEF_BIT(29, bypass_dm_data);
    DEF_BIT(30, bypass_dp_data);
    static auto Get(uint32_t i) {
        return hwreg::RegisterAddr<U2P_R0>(i * 0x20);
    }
};

class U2P_R1 : public hwreg::RegisterBase<U2P_R1, uint32_t> {
public:
    DEF_BIT(0, burn_in_test);
    DEF_BIT(1, aca_enable);
    DEF_BIT(2, dcd_enable);
    DEF_BIT(3, vdatsrcenb);
    DEF_BIT(4, vdatdetenb);
    DEF_BIT(5, chrgsel);
    DEF_BIT(6, tx_preemp_pulse_tune);
    DEF_FIELD(8, 7, tx_preemp_amp_tune);
    DEF_FIELD(10, 9, tx_res_tune);
    DEF_FIELD(12, 11, tx_rise_tune);
    DEF_FIELD(16, 13, tx_vref_tune);
    DEF_FIELD(20, 17, tx_fsls_tune);
    DEF_FIELD(22, 21, tx_hsxv_tune);
    DEF_FIELD(25, 23, otg_tune);
    DEF_FIELD(29, 26, sqrx_tune);
    DEF_FIELD(31, 29, comp_dis_tune);
    static auto Get(uint32_t i) {
        return hwreg::RegisterAddr<U2P_R1>(i * 0x20 + 4);
    }
};


class U2P_R2 : public hwreg::RegisterBase<U2P_R2, uint32_t> {
public:
    DEF_FIELD(3, 0, data_in);
    DEF_FIELD(7, 4, data_in_en);
    DEF_FIELD(11, 8, addr);
    DEF_BIT(12, data_out_sel);
    DEF_BIT(13, clk);
    DEF_FIELD(17, 14, data_out);
    DEF_BIT(18, aca_pin_range_c);
    DEF_BIT(19, aca_pin_range_b);
    DEF_BIT(20, aca_pin_range_a);
    DEF_BIT(21, aca_pin_gnd);
    DEF_BIT(22, aca_pin_float);
    DEF_BIT(23, chg_det);
    DEF_BIT(24, device_sess_vld);
    DEF_BIT(25, adp_probe);
    DEF_BIT(26, adp_sense);
    DEF_BIT(27, sessend);
    DEF_BIT(28, vbusvalid);
    DEF_BIT(29, bvalid);
    DEF_BIT(30, avalid);
    DEF_BIT(31, iddig);
    static auto Get(uint32_t i) {
        return hwreg::RegisterAddr<U2P_R2>(i * 0x20 + 8);
    }
};

class USB_R0 : public hwreg::RegisterBase<USB_R0, uint32_t> {
public:
    DEF_FIELD(5, 0, p30_fsel);
    DEF_BIT(6, p30_phy_reset);
    DEF_BIT(7, p30_test_powerdown_hsp);
    DEF_BIT(8, p30_test_powerdown_ssp);
    DEF_FIELD(13, 9, p30_acjt_level);
    DEF_FIELD(16, 14, p30_tx_vboost_lvl);
    DEF_BIT(17, p30_lane0_tx2rx_loopbk);
    DEF_BIT(18, p30_lane0_ext_pclk_req);
    DEF_FIELD(28, 19, p30_pcs_rx_los_mask_val);
    DEF_FIELD(30, 29, u2d_ss_scaledown_mode);
    DEF_BIT(31, u2d_act);
    static auto Get() {
        return hwreg::RegisterAddr<USB_R0>(0x80);
    }
};

class USB_R1 : public hwreg::RegisterBase<USB_R1, uint32_t> {
public:
    DEF_BIT(0, u3h_bigendian_gs);
    DEF_BIT(1, u3h_pme_en);
    DEF_FIELD(6, 2, u3h_hub_port_overcurrent);
    DEF_FIELD(11, 7, u3h_hub_port_perm_attach);
    DEF_FIELD(15, 12, u3h_host_u2_port_disable);
    DEF_BIT(16, u3h_host_u3_port_disable);
    DEF_BIT(17, u3h_host_port_power_control_present);
    DEF_BIT(18, u3h_host_msi_enable);
    DEF_FIELD(24, 19, u3h_fladj_30mhz_reg);
    DEF_FIELD(31, 25, p30_pcs_tx_swing_full);
    static auto Get() {
        return hwreg::RegisterAddr<USB_R1>(0x84);
    }
};

class USB_R2 : public hwreg::RegisterBase<USB_R2, uint32_t> {
public:
    DEF_FIELD(15, 0, p30_cr_data_in);
    DEF_BIT(16, p30_cr_read);
    DEF_BIT(17, p30_cr_write);
    DEF_BIT(18, p30_cr_cap_addr);
    DEF_BIT(19, p30_cr_cap_data);
    DEF_FIELD(25, 20, p30_pcs_tx_deemph_3p5db);
    DEF_FIELD(31, 26, p30_pcs_tx_deemph_6db);
    static auto Get() {
        return hwreg::RegisterAddr<USB_R2>(0x88);
    }
};

class USB_R3 : public hwreg::RegisterBase<USB_R3, uint32_t> {
public:
    DEF_BIT(0, p30_ssc_en);
    DEF_FIELD(3, 1, p30_ssc_range);
    DEF_FIELD(12, 4, p30_ssc_ref_clk_sel);
    DEF_BIT(13, p30_ref_ssp_en);
    DEF_FIELD(18, 16, p30_los_bias);
    DEF_FIELD(23, 19, p30_los_level);
    DEF_FIELD(30, 24, p30_mpll_multiplier);
    static auto Get() {
        return hwreg::RegisterAddr<USB_R3>(0x8c);
    }
};

class USB_R4 : public hwreg::RegisterBase<USB_R4, uint32_t> {
public:
    DEF_BIT(0, p21_portreset0);
    DEF_BIT(1, p21_sleepm0);
    DEF_FIELD(3, 2, mem_pd);
    DEF_BIT(4, p21_only);
    static auto Get() {
        return hwreg::RegisterAddr<USB_R4>(0x90);
    }
};

class USB_R5 : public hwreg::RegisterBase<USB_R5, uint32_t> {
public:
    DEF_BIT(0, iddig_sync);
    DEF_BIT(1, iddig_reg);
    DEF_FIELD(3, 2, iddig_cfg);
    DEF_BIT(4, iddig_en0);
    DEF_BIT(5, iddig_en1);
    DEF_BIT(6, iddig_curr);
    DEF_BIT(7, iddig_irq);
    DEF_FIELD(15, 8, iddig_th);
    DEF_FIELD(23, 16, iddig_cnt);
    static auto Get() {
        return hwreg::RegisterAddr<USB_R5>(0x94);
    }
};

class USB_R6 : public hwreg::RegisterBase<USB_R6, uint32_t> {
public:
    DEF_FIELD(15, 0, p30_cr_data_out);
    DEF_BIT(16, p30_cr_ack);
    static auto Get() {
        return hwreg::RegisterAddr<USB_R6>(0x98);
    }
};

#else // __cplusplus

// size of phy port register block
#define PHY_REGISTER_SIZE 32
#define U2P_R0_OFFSET   0
#define U2P_R1_OFFSET   4
#define U2P_R2_OFFSET   8

#define USB_R0_OFFSET   0
#define USB_R1_OFFSET   4
#define USB_R2_OFFSET   8
#define USB_R3_OFFSET   12
#define USB_R4_OFFSET   16
#define USB_R5_OFFSET   20
#define USB_R6_OFFSET   24

#define U2P_R0_BYPASS_SEL                       (1 << 0)
#define U2P_R0_BYPASS_DM_EN                     (1 << 1)
#define U2P_R0_BYPASS_DP_EN                     (1 << 2)
#define U2P_R0_TXBITSTUFFENH                    (1 << 3)
#define U2P_R0_TXBITSTUFFEN                     (1 << 4)
#define U2P_R0_DMPULLDOWN                       (1 << 5)
#define U2P_R0_DPPULLDOWN                       (1 << 6)
#define U2P_R0_VBUSVLDEXTSEL                    (1 << 7)
#define U2P_R0_VBUSVLDEXT                       (1 << 8)
#define U2P_R0_ADP_PRB_EN                       (1 << 9)
#define U2P_R0_ADP_DISCHRG                      (1 << 10)
#define U2P_R0_ADP_CHRG                         (1 << 11)
#define U2P_R0_DRVVBUS                          (1 << 12)
#define U2P_R0_IDPULLUP                         (1 << 13)
#define U2P_R0_LOOPBACKENB                      (1 << 14)
#define U2P_R0_OTGDISABLE                       (1 << 15)
#define U2P_R0_COMMONONN                        (1 << 16)
#define U2P_R0_FSEL_START                       17
#define U2P_R0_FSEL_BITS                        3
#define U2P_R0_REFCLKSEL_START                  20
#define U2P_R0_REFCLKSEL_BITS                   2
#define U2P_R0_POR                              (1 << 22)
#define U2P_R0_VATESTENB_START                  23
#define U2P_R0_VATESTENB_BITS                   2
#define U2P_R0_SET_IDDQ                         (1 << 25)
#define U2P_R0_ATE_RESET                        (1 << 26)
#define U2P_R0_FSV_MINUS                        (1 << 27)
#define U2P_R0_FSV_PLUS                         (1 << 28)
#define U2P_R0_BYPASS_DM_DATA                   (1 << 29)
#define U2P_R0_BYPASS_DP_DATA                   (1 << 30)

#define U2P_R1_BURN_IN_TEST                     (1 << 0)
#define U2P_R1_ACA_ENABLE                       (1 << 1)
#define U2P_R1_DCD_ENABLE                       (1 << 2)
#define U2P_R1_VDATSRCENB                       (1 << 3)
#define U2P_R1_VDATDETENB                       (1 << 4)
#define U2P_R1_CHRGSEL                          (1 << 5)
#define U2P_R1_TX_PREEMP_PULSE_TUNE             (1 << 6)
#define U2P_R1_TX_PREEMP_AMP_TUNE_START         7
#define U2P_R1_TX_PREEMP_AMP_TUNE_BITS          2
#define U2P_R1_TX_RES_TUNE_START                9
#define U2P_R1_TX_RES_TUNE_BITS                 2
#define U2P_R1_TX_RISE_TUNE_START               11
#define U2P_R1_TX_RISE_TUNE_BITS                2
#define U2P_R1_TX_VREF_TUNE_START               13
#define U2P_R1_TX_VREF_TUNE_BITS                4
#define U2P_R1_TX_FSLS_TUNE_START               17
#define U2P_R1_TX_FSLS_TUNE_BITS                4
#define U2P_R1_TX_HSXV_TUNE_START               21
#define U2P_R1_TX_HSXV_TUNE_BITS                2
#define U2P_R1_OTG_TUNE_START                   23
#define U2P_R1_OTG_TUNE_BITS                    3
#define U2P_R1_SQRX_TUNE_START                  26
#define U2P_R1_SQRX_TUNE_BITS                   3
#define U2P_R1_COMP_DIS_TUNE_START              29
#define U2P_R1_COMP_DIS_TUNE_BITS               3

#define U2P_R2_DATA_IN_START                    0
#define U2P_R2_DATA_IN_BITS                     4
#define U2P_R2_DATA_IN_EN_START                 4
#define U2P_R2_DATA_IN_EN_BITS                  4
#define U2P_R2_ADDR_START                       8
#define U2P_R2_ADDR_BITS                        4
#define U2P_R2_DATA_OUT_SEL                     (1 << 12)
#define U2P_R2_CLK                              (1 << 13)
#define U2P_R2_DATA_OUT_START                    14
#define U2P_R2_DATA_OUT_BITS                     4
#define U2P_R2_ACA_PIN_RANGE_C                  (1 << 18)
#define U2P_R2_ACA_PIN_RANGE_B                  (1 << 19)
#define U2P_R2_ACA_PIN_RANGE_A                  (1 << 20)
#define U2P_R2_ACA_PIN_GND                      (1 << 21)
#define U2P_R2_ACA_PIN_FLOAT                    (1 << 22)
#define U2P_R2_CHG_DET                          (1 << 23)
#define U2P_R2_DEVICE_SESS_VLD                  (1 << 24)
#define U2P_R2_ADP_PROBE                        (1 << 25)
#define U2P_R2_ADP_SENSE                        (1 << 26)
#define U2P_R2_SESSEND                          (1 << 27)
#define U2P_R2_VBUSVALID                        (1 << 28)
#define U2P_R2_BVALID                           (1 << 29)
#define U2P_R2_AVALID                           (1 << 30)
#define U2P_R2_IDDIG                            (1 << 31)

#define USB_R0_P30_FSEL_START                   0
#define USB_R0_P30_FSEL_BITS                    6
#define USB_R0_P30_PHY_RESET                    (1 << 6)
#define USB_R0_P30_TEST_POWERDOWN_HSP           (1 << 7)
#define USB_R0_P30_TEST_POWERDOWN_SSP           (1 << 8)
#define USB_R0_P30_ACJT_LEVEL_START             9
#define USB_R0_P30_ACJT_LEVEL_BITS              5
#define USB_R0_P30_TX_VBOOST_LVL_START          14
#define USB_R0_P30_TX_VBOOST_LVL_BITS           3
#define USB_R0_P30_LANE0_TX2RX_LOOPBK           (1 << 17)
#define USB_R0_P30_LANE0_EXT_PCLK_REQ           (1 << 18)
#define USB_R0_P30_PCS_RX_LOS_MASK_VAL_START    19
#define USB_R0_P30_PCS_RX_LOS_MASK_VAL_BITS     10
#define USB_R0_U2D_SS_SCALEDOWN_MODE_START      29
#define USB_R0_U2D_SS_SCALEDOWN_MODE_BITS       2
#define USB_R0_U2D_ACT                          (1 << 31)

#define USB_R1_U3H_BIGENDIAN_GS                 (1 << 0)
#define USB_R1_U3H_PME_EN                       (1 << 1)
#define USB_R1_U3H_HUB_PORT_OVERCURRENT_START   2
#define USB_R1_U3H_HUB_PORT_OVERCURRENT_BITS    5
#define USB_R1_U3H_HUB_PORT_PERM_ATTACH_START   7
#define USB_R1_U3H_HUB_PORT_PERM_ATTACH_BITS    5
#define USB_R1_U3H_HOST_U2_PORT_DISABLE_START   12
#define USB_R1_U3H_HOST_U2_PORT_DISABLE_BITS    4
#define USB_R1_U3H_HOST_U3_PORT_DISABLE         (1 << 16)
#define USB_R1_U3H_HOST_PORT_POWER_CONTROL_PRESENT  (1 << 17)
#define USB_R1_U3H_HOST_MSI_ENABLE              (1 << 18)
#define USB_R1_U3H_FLADJ_30MHZ_REG_START        19
#define USB_R1_U3H_FLADJ_30MHZ_REG_BITS         6
#define USB_R1_P30_PCS_TX_SWING_FULL_START      25
#define USB_R1_P30_PCS_TX_SWING_FULL            7

#define USB_R2_P30_CR_DATA_IN_START             0
#define USB_R2_P30_CR_DATA_IN_BITS              16
#define USB_R2_P30_CR_READ                      (1 << 16)
#define USB_R2_P30_CR_WRITE                     (1 << 17)
#define USB_R2_P30_CR_CAP_ADDR                  (1 << 18)
#define USB_R2_P30_CR_CAP_DATA                  (1 << 19)
#define USB_R2_P30_PCS_TX_DEEMPH_3P5DB_START    20
#define USB_R2_P30_PCS_TX_DEEMPH_3P5DB_BITS     6
#define USB_R2_P30_PCS_TX_DEEMPH_6DB_START      26
#define USB_R2_P30_PCS_TX_DEEMPH_6DB_BITS       6

#define USB_R3_P30_SSC_EN                       (1 << 0)
#define USB_R3_P30_SSC_RANGE_START              1
#define USB_R3_P30_SSC_RANGE_BITS               3
#define USB_R3_P30_SSC_REF_CLK_SEL_START        4
#define USB_R3_P30_SSC_REF_CLK_SEL_BITS         9
#define USB_R3_P30_REF_SSP_EN                   (1 << 13)
#define USB_R3_RESERVED14_START                 14
#define USB_R3_RESERVED14_BITS                  2
#define USB_R3_P30_LOS_BIAS_START               16
#define USB_R3_P30_LOS_BIAS_BITS                3
#define USB_R3_P30_LOS_LEVEL_START              19
#define USB_R3_P30_LOS_LEVEL_BITS               5
#define USB_R3_P30_MPLL_MULTIPLIER_START        24
#define USB_R3_P30_MPLL_MULTIPLIER_BITS         7

#define USB_R4_P21_PORTRESET0                   (1 << 0)
#define USB_R4_P21_SLEEPM0                      (1 << 1)
#define USB_R4_MEM_PD_START                     2
#define USB_R4_MEM_PD_BITS                      2
#define USB_R4_P21_ONLY                         (1 << 4)

#define USB_R5_IDDIG_SYNC                       (1 << 0)
#define USB_R5_IDDIG_REG                        (1 << 1)
#define USB_R5_IDDIG_CFG_START                  2
#define USB_R5_IDDIG_CFG_BITS                   2
#define USB_R5_IDDIG_EN0                        (1 << 4)
#define USB_R5_IDDIG_EN1                        (1 << 5)
#define USB_R5_IDDIG_CURR                       (1 << 6)
#define USB_R5_IDDIG_IRQ                        (1 << 7)
#define USB_R5_IDDIG_TH_START                   8
#define USB_R5_IDDIG_TH_BITS                    8
#define USB_R5_IDDIG_CNT_START                  16
#define USB_R5_IDDIG_CNT_BITS                   8

#define USB_R6_P30_CR_DATA_OUT_START            0
#define USB_R6_P30_CR_DATA_OUT_BITS             16
#define USB_R6_P30_CR_ACK                       (1 << 16)

#endif // __cplusplus
