// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msm8x53-clk.h"

#include <ddk/binding.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/clockimpl.h>
#include <ddk/protocol/platform/bus.h>
#include <ddk/protocol/platform/device.h>
#include <ddktl/pdev.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/hardware/clock/c/fidl.h>
#include <hwreg/bitfields.h>

#include <ddktl/protocol/platform/bus.h>

#include "msm8x53-clk-regs.h"

namespace clk {

typedef struct msm_clk_gate {
    uint32_t reg;
    uint32_t bit;
    uint32_t delay_us;
} msm_clk_gate_t;

struct msm_clk_branch {
    uint32_t reg;
};

struct msm_clk_voter {
    uint32_t cbcr_reg;
    uint32_t vote_reg;
    uint32_t bit;
};

enum class RcgDividerType {
    HalfInteger,
    Mnd
};

class RcgFrequencyTable {
    static constexpr uint32_t kPredivMask = 0x1f;
    static constexpr uint32_t kSrcMask = 0x7;
    static constexpr uint32_t kSrcShift = 8;
public:
    constexpr RcgFrequencyTable(uint64_t rate, uint32_t m, uint32_t n, uint32_t d2, uint32_t parent)
        : rate_(rate)
        , m_(m)
        , n_(n == 0 ? 0 : ~(n - m))
        , d_(~n)
        , predev_parent_(((d2 - 1) & kPredivMask) | ((parent & kSrcMask) << kSrcShift)) {}

        uint64_t rate() const { return rate_; }
        uint32_t m() const { return m_; }
        uint32_t n() const { return n_; }
        uint32_t d() const { return d_; }
        uint32_t predev_parent() const { return predev_parent_; }
private:
    const uint64_t rate_;
    const uint32_t m_;
    const uint32_t n_;
    const uint32_t d_;
    const uint32_t predev_parent_;
};

typedef struct msm_clk_rcg {
    uint32_t cmd_rcgr_reg;

    // Certain msm_clk_rcgs are currently unsupported. Specifically:
    // (1) Any RCG that derives its clock source from a dynamic PLL.
    // (2) Any RCG that has non-local children.
    // (3) Any RGC that has a non-local timeout.
    // Currently this appears to be only the kGfx3dClkSrc RCG for the MSM8x53.
    bool unsupported;
    RcgDividerType type;

    const RcgFrequencyTable* frequency_table;
    const size_t frequency_table_count;
} msm_clk_rcg_t;


namespace {

constexpr char kMsmClkName[] = "msm-clk";
constexpr uint32_t kRcgUpdateTimeoutUsec = 500;
constexpr uint64_t kRcgRateUnset = 0;

class RcgClkCmd : public hwreg::RegisterBase<RcgClkCmd, uint32_t> {
public:
    DEF_BIT(0, cfg_update);
    DEF_BIT(1, root_enable);
    DEF_BIT(31, root_status);

    static auto Read(uint32_t offset) {
        return hwreg::RegisterAddr<RcgClkCmd>(offset);
    }
};

constexpr msm_clk_gate_t kMsmClkGates[] = {
    [msm8x53::MsmClkIndex(msm8x53::kQUsbRefClk)] = {.reg = 0x41030, .bit = 0, .delay_us = 0},
    [msm8x53::MsmClkIndex(msm8x53::kUsbSSRefClk)] = {.reg = 0x5e07c, .bit = 0, .delay_us = 0},
    [msm8x53::MsmClkIndex(msm8x53::kUsb3PipeClk)] = {.reg = 0x5e040, .bit = 0, .delay_us = 50},
};

constexpr uint32_t kBranchEnable = (0x1u << 0);
constexpr struct msm_clk_branch kMsmClkBranches[] = {
    [msm8x53::MsmClkIndex(msm8x53::kApc0DroopDetectorGpll0Clk)] = {
        .reg = msm8x53::kApc0VoltageDroopDetectorGpll0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kApc1DroopDetectorGpll0Clk)] = {
        .reg = msm8x53::kApc1VoltageDroopDetectorGpll0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup1I2cAppsClk)] = {.reg = msm8x53::kBlsp1Qup1I2cAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup1SpiAppsClk)] = {.reg = msm8x53::kBlsp1Qup1SpiAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup2I2cAppsClk)] = {.reg = msm8x53::kBlsp1Qup2I2cAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup2SpiAppsClk)] = {.reg = msm8x53::kBlsp1Qup2SpiAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup3I2cAppsClk)] = {.reg = msm8x53::kBlsp1Qup3I2cAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup3SpiAppsClk)] = {.reg = msm8x53::kBlsp1Qup3SpiAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup4I2cAppsClk)] = {.reg = msm8x53::kBlsp1Qup4I2cAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup4SpiAppsClk)] = {.reg = msm8x53::kBlsp1Qup4SpiAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Uart1AppsClk)] = {.reg = msm8x53::kBlsp1Uart1AppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Uart2AppsClk)] = {.reg = msm8x53::kBlsp1Uart2AppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup1I2cAppsClk)] = {.reg = msm8x53::kBlsp2Qup1I2cAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup1SpiAppsClk)] = {.reg = msm8x53::kBlsp2Qup1SpiAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup2I2cAppsClk)] = {.reg = msm8x53::kBlsp2Qup2I2cAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup2SpiAppsClk)] = {.reg = msm8x53::kBlsp2Qup2SpiAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup3I2cAppsClk)] = {.reg = msm8x53::kBlsp2Qup3I2cAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup3SpiAppsClk)] = {.reg = msm8x53::kBlsp2Qup3SpiAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup4I2cAppsClk)] = {.reg = msm8x53::kBlsp2Qup4I2cAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup4SpiAppsClk)] = {.reg = msm8x53::kBlsp2Qup4SpiAppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Uart1AppsClk)] = {.reg = msm8x53::kBlsp2Uart1AppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Uart2AppsClk)] = {.reg = msm8x53::kBlsp2Uart2AppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBimcGpuClk)] = {.reg = msm8x53::kBimcGpuCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCciAhbClk)] = {.reg = msm8x53::kCamssCciAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCciClk)] = {.reg = msm8x53::kCamssCciCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCppAhbClk)] = {.reg = msm8x53::kCamssCppAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCppAxiClk)] = {.reg = msm8x53::kCamssCppAxiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCppClk)] = {.reg = msm8x53::kCamssCppCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi0AhbClk)] = {.reg = msm8x53::kCamssCsi0AhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi0Clk)] = {.reg = msm8x53::kCamssCsi0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi0Csiphy3pClk)] = {
        .reg = msm8x53::kCamssCsi0Csiphy3pCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi0phyClk)] = {.reg = msm8x53::kCamssCsi0phyCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi0pixClk)] = {.reg = msm8x53::kCamssCsi0pixCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi0rdiClk)] = {.reg = msm8x53::kCamssCsi0rdiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi1AhbClk)] = {.reg = msm8x53::kCamssCsi1AhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi1Clk)] = {.reg = msm8x53::kCamssCsi1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi1Csiphy3pClk)] = {
        .reg = msm8x53::kCamssCsi1Csiphy3pCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi1phyClk)] = {.reg = msm8x53::kCamssCsi1phyCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi1pixClk)] = {.reg = msm8x53::kCamssCsi1pixCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi1rdiClk)] = {.reg = msm8x53::kCamssCsi1rdiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi2AhbClk)] = {.reg = msm8x53::kCamssCsi2AhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi2Clk)] = {.reg = msm8x53::kCamssCsi2Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi2Csiphy3pClk)] = {
        .reg = msm8x53::kCamssCsi2Csiphy3pCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi2phyClk)] = {.reg = msm8x53::kCamssCsi2phyCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi2pixClk)] = {.reg = msm8x53::kCamssCsi2pixCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi2rdiClk)] = {.reg = msm8x53::kCamssCsi2rdiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsiVfe0Clk)] = {.reg = msm8x53::kCamssCsiVfe0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsiVfe1Clk)] = {.reg = msm8x53::kCamssCsiVfe1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssGp0Clk)] = {.reg = msm8x53::kCamssGp0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssGp1Clk)] = {.reg = msm8x53::kCamssGp1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssIspifAhbClk)] = {.reg = msm8x53::kCamssIspifAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssJpeg0Clk)] = {.reg = msm8x53::kCamssJpeg0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssJpegAhbClk)] = {.reg = msm8x53::kCamssJpegAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssJpegAxiClk)] = {.reg = msm8x53::kCamssJpegAxiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssMclk0Clk)] = {.reg = msm8x53::kCamssMclk0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssMclk1Clk)] = {.reg = msm8x53::kCamssMclk1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssMclk2Clk)] = {.reg = msm8x53::kCamssMclk2Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssMclk3Clk)] = {.reg = msm8x53::kCamssMclk3Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssMicroAhbClk)] = {.reg = msm8x53::kCamssMicroAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi0phytimerClk)] = {
        .reg = msm8x53::kCamssCsi0phytimerCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi1phytimerClk)] = {
        .reg = msm8x53::kCamssCsi1phytimerCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssCsi2phytimerClk)] = {
        .reg = msm8x53::kCamssCsi2phytimerCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssAhbClk)] = {.reg = msm8x53::kCamssAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssTopAhbClk)] = {.reg = msm8x53::kCamssTopAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssVfe0Clk)] = {.reg = msm8x53::kCamssVfe0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssVfeAhbClk)] = {.reg = msm8x53::kCamssVfeAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssVfeAxiClk)] = {.reg = msm8x53::kCamssVfeAxiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssVfe1AhbClk)] = {.reg = msm8x53::kCamssVfe1AhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssVfe1AxiClk)] = {.reg = msm8x53::kCamssVfe1AxiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kCamssVfe1Clk)] = {.reg = msm8x53::kCamssVfe1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kDccClk)] = {.reg = msm8x53::kDccCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kGp1Clk)] = {.reg = msm8x53::kGp1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kGp2Clk)] = {.reg = msm8x53::kGp2Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kGp3Clk)] = {.reg = msm8x53::kGp3Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssAhbClk)] = {.reg = msm8x53::kMdssAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssAxiClk)] = {.reg = msm8x53::kMdssAxiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssByte0Clk)] = {.reg = msm8x53::kMdssByte0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssByte1Clk)] = {.reg = msm8x53::kMdssByte1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssEsc0Clk)] = {.reg = msm8x53::kMdssEsc0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssEsc1Clk)] = {.reg = msm8x53::kMdssEsc1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssMdpClk)] = {.reg = msm8x53::kMdssMdpCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssPclk0Clk)] = {.reg = msm8x53::kMdssPclk0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssPclk1Clk)] = {.reg = msm8x53::kMdssPclk1Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMdssVsyncClk)] = {.reg = msm8x53::kMdssVsyncCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMssCfgAhbClk)] = {.reg = msm8x53::kMssCfgAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kMssQ6BimcAxiClk)] = {.reg = msm8x53::kMssQ6BimcAxiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kBimcGfxClk)] = {.reg = msm8x53::kBimcGfxCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kOxiliAhbClk)] = {.reg = msm8x53::kOxiliAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kOxiliAonClk)] = {.reg = msm8x53::kOxiliAonCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kOxiliGfx3dClk)] = {.reg = msm8x53::kOxiliGfx3dCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kOxiliTimerClk)] = {.reg = msm8x53::kOxiliTimerCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kPcnocUsb3AxiClk)] = {.reg = msm8x53::kPcnocUsb3AxiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kPdm2Clk)] = {.reg = msm8x53::kPdm2Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kPdmAhbClk)] = {.reg = msm8x53::kPdmAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kRbcprGfxClk)] = {.reg = msm8x53::kRbcprGfxCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kSdcc1AhbClk)] = {.reg = msm8x53::kSdcc1AhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kSdcc1AppsClk)] = {.reg = msm8x53::kSdcc1AppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kSdcc1IceCoreClk)] = {.reg = msm8x53::kSdcc1IceCoreCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kSdcc2AhbClk)] = {.reg = msm8x53::kSdcc2AhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kSdcc2AppsClk)] = {.reg = msm8x53::kSdcc2AppsCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kUsb30MasterClk)] = {.reg = msm8x53::kUsb30MasterCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kUsb30MockUtmiClk)] = {.reg = msm8x53::kUsb30MockUtmiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kUsb30SleepClk)] = {.reg = msm8x53::kUsb30SleepCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kUsb3AuxClk)] = {.reg = msm8x53::kUsb3AuxCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kUsbPhyCfgAhbClk)] = {.reg = msm8x53::kUsbPhyCfgAhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kVenus0AhbClk)] = {.reg = msm8x53::kVenus0AhbCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kVenus0AxiClk)] = {.reg = msm8x53::kVenus0AxiCbcr},
    [msm8x53::MsmClkIndex(msm8x53::kVenus0Core0Vcodec0Clk)] = {
        .reg = msm8x53::kVenus0Core0Vcodec0Cbcr},
    [msm8x53::MsmClkIndex(msm8x53::kVenus0Vcodec0Clk)] = {.reg = msm8x53::kVenus0Vcodec0Cbcr},
};

constexpr RcgFrequencyTable kFtblCamssTopAhbClkSrc[] = {
    RcgFrequencyTable(40000000, 0, 0, 20, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(80000000, 0, 0, 20, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblCsi0ClkSrc[] = {
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2SrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(465000000, 0, 0, 4, msm8x53::kGpll2SrcVal),
};

constexpr RcgFrequencyTable kFtblApssAhbClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoASrcVal),
    RcgFrequencyTable(25000000, 0, 0, 32, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(50000000, 0, 0, 32, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(100000000, 0, 0, 16, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(133330000, 0, 0, 12, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblCsi1ClkSrc[] = {
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2OutMainSrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(465000000, 0, 0, 4, msm8x53::kGpll2OutMainSrcVal),
};

constexpr RcgFrequencyTable kFtblCsi2ClkSrc[] = {
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2OutMainSrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(465000000, 0, 0, 4, msm8x53::kGpll2OutMainSrcVal),
};

constexpr RcgFrequencyTable kFtblVfe0ClkSrc[] = {
    RcgFrequencyTable(50000000, 0, 0, 16, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(133330000, 0, 0, 12, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(160000000, 0, 0, 10, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2SrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(465000000, 0, 0, 4, msm8x53::kGpll2SrcVal),
};

constexpr RcgFrequencyTable kFtblGfx3dClkSrc[] = {
};

constexpr RcgFrequencyTable kFtblGfx3dClkSrcSdm450[] = {
};

constexpr RcgFrequencyTable kFtblGfx3dClkSrcSdm632[] = {
};

constexpr RcgFrequencyTable kFtblVcodec0ClkSrc[] = {
    RcgFrequencyTable(114290000, 0, 0, 7, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(228570000, 0, 0, 7, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2VcodecSrcVal),
    RcgFrequencyTable(360000000, 0, 0, 6, msm8x53::kGpll6SrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(465000000, 0, 0, 4, msm8x53::kGpll2VcodecSrcVal),
};

constexpr RcgFrequencyTable kFtblVcodec0ClkSrc540MHz[] = {
    RcgFrequencyTable(114290000, 0, 0, 7, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(228570000, 0, 0, 7, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2VcodecSrcVal),
    RcgFrequencyTable(360000000, 0, 0, 6, msm8x53::kGpll6SrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(465000000, 0, 0, 4, msm8x53::kGpll2VcodecSrcVal),
    RcgFrequencyTable(540000000, 0, 0, 4, msm8x53::kGpll6SrcVal),
};

constexpr RcgFrequencyTable kFtblCppClkSrc[] = {
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(320000000, 0, 0, 5, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(465000000, 0, 0, 4, msm8x53::kGpll2SrcVal),
};

constexpr RcgFrequencyTable kFtblJpeg0ClkSrc[] = {
    RcgFrequencyTable(66670000, 0, 0, 12, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(133330000, 0, 0, 12, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2OutMainSrcVal),
    RcgFrequencyTable(320000000, 0, 0, 5, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblMdpClkSrc[] = {
    RcgFrequencyTable(50000000, 0, 0, 16, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(80000000, 0, 0, 10, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(160000000, 0, 0, 5, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(320000000, 0, 0, 5, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblPclk0ClkSrc[] = {
};

constexpr RcgFrequencyTable kFtblPclk1ClkSrc[] = {
};

constexpr RcgFrequencyTable kFtblUsb30MasterClkSrc[] = {
    RcgFrequencyTable(80000000, 0, 0, 10, msm8x53::kGpll0MainDiv2Usb3SrcVal),
    RcgFrequencyTable(100000000, 0, 0, 16, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(133330000, 0, 0, 12, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblVfe1ClkSrc[] = {
    RcgFrequencyTable(50000000, 0, 0, 16, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(133330000, 0, 0, 12, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(160000000, 0, 0, 10, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2SrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(465000000, 0, 0, 4, msm8x53::kGpll2SrcVal),
};

constexpr RcgFrequencyTable kFtblApc0DroopDetectorClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(576000000, 0, 0, 4, msm8x53::kGpll4SrcVal),
};

constexpr RcgFrequencyTable kFtblApc1DroopDetectorClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
    RcgFrequencyTable(400000000, 0, 0, 4, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(576000000, 0, 0, 4, msm8x53::kGpll4SrcVal),
};

constexpr RcgFrequencyTable kFtblBlspI2cAppsClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
    RcgFrequencyTable(25000000, 0, 0, 32, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(50000000, 0, 0, 32, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblBlspSpiAppsClkSrc[] = {
    RcgFrequencyTable(960000, 1, 2, 20, msm8x53::kXoSrcVal),
    RcgFrequencyTable(4800000, 0, 0, 8, msm8x53::kXoSrcVal),
    RcgFrequencyTable(9600000, 0, 0, 4, msm8x53::kXoSrcVal),
    RcgFrequencyTable(12500000, 1, 2, 32, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(16000000, 1, 5, 20, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
    RcgFrequencyTable(25000000, 1, 2, 32, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(50000000, 0, 0, 32, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblBlspUartAppsClkSrc[] = {
    RcgFrequencyTable(3686400, 144, 15625, 2, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(7372800, 288, 15625, 2, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(14745600, 576, 15625, 2, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(16000000, 1, 5, 10, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
    RcgFrequencyTable(24000000, 3, 100, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(25000000, 1, 2, 32, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(32000000, 1, 25, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(40000000, 1, 20, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(46400000, 29, 500, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(48000000, 3, 50, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(51200000, 8, 125, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(56000000, 7, 100, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(58982400, 1152, 15625, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(60000000, 3, 40, 2, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(64000000, 2, 25, 2, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblCciClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
    RcgFrequencyTable(37500000, 3, 32, 2, msm8x53::kGpll0MainDiv2CciSrcVal),
};

constexpr RcgFrequencyTable kFtblCsi0pClkSrc[] = {
    RcgFrequencyTable(66670000, 0, 0, 12, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(133330000, 0, 0, 12, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2SrcVal),
};

constexpr RcgFrequencyTable kFtblCsi1pClkSrc[] = {
    RcgFrequencyTable(66670000, 0, 0, 12, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(133330000, 0, 0, 12, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2SrcVal),
};

constexpr RcgFrequencyTable kFtblCsi2pClkSrc[] = {
    RcgFrequencyTable(66670000, 0, 0, 12, msm8x53::kGpll0MainDiv2MmSrcVal),
    RcgFrequencyTable(133330000, 0, 0, 12, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(310000000, 0, 0, 6, msm8x53::kGpll2SrcVal),
};

constexpr RcgFrequencyTable kFtblCamssGp0ClkSrc[] = {
    RcgFrequencyTable(50000000, 0, 0, 16, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(100000000, 0, 0, 16, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblCamssGp1ClkSrc[] = {
    RcgFrequencyTable(50000000, 0, 0, 16, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(100000000, 0, 0, 16, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblMclk0ClkSrc[] = {
    RcgFrequencyTable(24000000, 2, 45, 2, msm8x53::kGpll6MainDiv2SrcVal),
    RcgFrequencyTable(33330000, 0, 0, 24, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(36610000, 2, 59, 2, msm8x53::kGpll6SrcVal),
    RcgFrequencyTable(66667000, 0, 0, 24, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblMclk1ClkSrc[] = {
    RcgFrequencyTable(24000000, 2, 45, 2, msm8x53::kGpll6MainDiv2SrcVal),
    RcgFrequencyTable(33330000, 0, 0, 24, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(36610000, 2, 59, 2, msm8x53::kGpll6SrcVal),
    RcgFrequencyTable(66667000, 0, 0, 24, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblMclk2ClkSrc[] = {
    RcgFrequencyTable(24000000, 2, 45, 2, msm8x53::kGpll6MainDiv2SrcVal),
    RcgFrequencyTable(33330000, 0, 0, 24, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(36610000, 2, 59, 2, msm8x53::kGpll6SrcVal),
    RcgFrequencyTable(66667000, 0, 0, 24, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblMclk3ClkSrc[] = {
    RcgFrequencyTable(24000000, 2, 45, 2, msm8x53::kGpll6MainDiv2SrcVal),
    RcgFrequencyTable(33330000, 0, 0, 24, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(36610000, 2, 59, 2, msm8x53::kGpll6SrcVal),
    RcgFrequencyTable(66667000, 0, 0, 24, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblCsi0phytimerClkSrc[] = {
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblCsi1phytimerClkSrc[] = {
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblCsi2phytimerClkSrc[] = {
    RcgFrequencyTable(100000000, 0, 0, 8, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(266670000, 0, 0, 6, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblCryptoClkSrc[] = {
    RcgFrequencyTable(40000000, 0, 0, 20, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(80000000, 0, 0, 20, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(100000000, 0, 0, 16, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(160000000, 0, 0, 10, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblGp1ClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
};

constexpr RcgFrequencyTable kFtblGp2ClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
};

constexpr RcgFrequencyTable kFtblGp3ClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
};

constexpr RcgFrequencyTable kFtblByte0ClkSrc[] = {
};

constexpr RcgFrequencyTable kFtblByte1ClkSrc[] = {
};

constexpr RcgFrequencyTable kFtblEsc0ClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
};

constexpr RcgFrequencyTable kFtblEsc1ClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
};

constexpr RcgFrequencyTable kFtblVsyncClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
};

constexpr RcgFrequencyTable kFtblPdm2ClkSrc[] = {
    RcgFrequencyTable(32000000, 0, 0, 25, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(64000000, 0, 0, 25, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblRbcprGfxClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
    RcgFrequencyTable(50000000, 0, 0, 32, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblSdcc1AppsClkSrc[] = {
    RcgFrequencyTable(144000, 3, 25, 32, msm8x53::kXoSrcVal),
    RcgFrequencyTable(400000, 1, 4, 24, msm8x53::kXoSrcVal),
    RcgFrequencyTable(20000000, 1, 4, 10, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(25000000, 0, 0, 32, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(50000000, 0, 0, 32, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(100000000, 0, 0, 16, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(177770000, 0, 0, 9, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(192000000, 0, 0, 12, msm8x53::kGpll4SrcVal),
    RcgFrequencyTable(384000000, 0, 0, 6, msm8x53::kGpll4SrcVal),
};

constexpr RcgFrequencyTable kFtblSdcc1IceCoreClkSrc[] = {
    RcgFrequencyTable(80000000, 0, 0, 10, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(160000000, 0, 0, 10, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(270000000, 0, 0, 8, msm8x53::kGpll6SrcVal),
};

constexpr RcgFrequencyTable kFtblSdcc2AppsClkSrc[] = {
    RcgFrequencyTable(144000, 3, 25, 32, msm8x53::kXoSrcVal),
    RcgFrequencyTable(400000, 1, 4, 24, msm8x53::kXoSrcVal),
    RcgFrequencyTable(20000000, 1, 4, 10, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(25000000, 0, 0, 32, msm8x53::kGpll0MainDiv2SrcVal),
    RcgFrequencyTable(50000000, 0, 0, 32, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(100000000, 0, 0, 16, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(177770000, 0, 0, 9, msm8x53::kGpll0SrcVal),
    RcgFrequencyTable(192000000, 0, 0, 12, msm8x53::kGpll4AuxSrcVal),
    RcgFrequencyTable(200000000, 0, 0, 8, msm8x53::kGpll0SrcVal),
};

constexpr RcgFrequencyTable kFtblUsb30MockUtmiClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
    RcgFrequencyTable(60000000, 1, 1, 18, msm8x53::kGpll6MainDiv2MockSrcVal),
};

constexpr RcgFrequencyTable kFtblUsb3AuxClkSrc[] = {
    RcgFrequencyTable(19200000, 0, 0, 2, msm8x53::kXoSrcVal),
};

constexpr struct msm_clk_rcg kMsmClkRcgs[] = {
    [msm8x53::MsmClkIndex(msm8x53::kCamssTopAhbClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCamssTopAhbCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblCamssTopAhbClkSrc,
        .frequency_table_count = countof(kFtblCamssTopAhbClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi0ClkSrc,
        .frequency_table_count = countof(kFtblCsi0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kApssAhbClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kApssAhbCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblApssAhbClkSrc,
        .frequency_table_count = countof(kFtblApssAhbClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi1ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi1CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi1ClkSrc,
        .frequency_table_count = countof(kFtblCsi1ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi2ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi2CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi2ClkSrc,
        .frequency_table_count = countof(kFtblCsi2ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kVfe0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kVfe0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblVfe0ClkSrc,
        .frequency_table_count = countof(kFtblVfe0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kGfx3dClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kGfx3dCmdRcgr,
        .unsupported = true,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblGfx3dClkSrc,
        .frequency_table_count = countof(kFtblGfx3dClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kVcodec0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kVcodec0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblVcodec0ClkSrc,
        .frequency_table_count = countof(kFtblVcodec0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCppClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCppCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCppClkSrc,
        .frequency_table_count = countof(kFtblCppClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kJpeg0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kJpeg0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblJpeg0ClkSrc,
        .frequency_table_count = countof(kFtblJpeg0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kMdpClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kMdpCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblMdpClkSrc,
        .frequency_table_count = countof(kFtblMdpClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kPclk0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kPclk0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblPclk0ClkSrc,
        .frequency_table_count = countof(kFtblPclk0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kPclk1ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kPclk1CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblPclk1ClkSrc,
        .frequency_table_count = countof(kFtblPclk1ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kUsb30MasterClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kUsb30MasterCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblUsb30MasterClkSrc,
        .frequency_table_count = countof(kFtblUsb30MasterClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kVfe1ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kVfe1CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblVfe1ClkSrc,
        .frequency_table_count = countof(kFtblVfe1ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kApc0DroopDetectorClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kApc0VoltageDroopDetectorCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblApc0DroopDetectorClkSrc,
        .frequency_table_count = countof(kFtblApc0DroopDetectorClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kApc1DroopDetectorClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kApc1VoltageDroopDetectorCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblApc1DroopDetectorClkSrc,
        .frequency_table_count = countof(kFtblApc1DroopDetectorClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup1I2cAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Qup1I2cAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblBlspI2cAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspI2cAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup1SpiAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Qup1SpiAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspSpiAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspSpiAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup2I2cAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Qup2I2cAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblBlspI2cAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspI2cAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup2SpiAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Qup2SpiAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspSpiAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspSpiAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup3I2cAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Qup3I2cAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblBlspI2cAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspI2cAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup3SpiAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Qup3SpiAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspSpiAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspSpiAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup4I2cAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Qup4I2cAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblBlspI2cAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspI2cAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Qup4SpiAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Qup4SpiAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspSpiAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspSpiAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Uart1AppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Uart1AppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspUartAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspUartAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1Uart2AppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp1Uart2AppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspUartAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspUartAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup1I2cAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Qup1I2cAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblBlspI2cAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspI2cAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup1SpiAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Qup1SpiAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspSpiAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspSpiAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup2I2cAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Qup2I2cAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblBlspI2cAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspI2cAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup2SpiAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Qup2SpiAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspSpiAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspSpiAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup3I2cAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Qup3I2cAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblBlspI2cAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspI2cAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup3SpiAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Qup3SpiAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspSpiAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspSpiAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup4I2cAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Qup4I2cAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblBlspI2cAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspI2cAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Qup4SpiAppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Qup4SpiAppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspSpiAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspSpiAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Uart1AppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Uart1AppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspUartAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspUartAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2Uart2AppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kBlsp2Uart2AppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblBlspUartAppsClkSrc,
        .frequency_table_count = countof(kFtblBlspUartAppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCciClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCciCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblCciClkSrc,
        .frequency_table_count = countof(kFtblCciClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi0pClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi0pCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi0pClkSrc,
        .frequency_table_count = countof(kFtblCsi0pClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi1pClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi1pCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi1pClkSrc,
        .frequency_table_count = countof(kFtblCsi1pClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi2pClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi2pCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi2pClkSrc,
        .frequency_table_count = countof(kFtblCsi2pClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCamssGp0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCamssGp0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblCamssGp0ClkSrc,
        .frequency_table_count = countof(kFtblCamssGp0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCamssGp1ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCamssGp1CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblCamssGp1ClkSrc,
        .frequency_table_count = countof(kFtblCamssGp1ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kMclk0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kMclk0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblMclk0ClkSrc,
        .frequency_table_count = countof(kFtblMclk0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kMclk1ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kMclk1CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblMclk1ClkSrc,
        .frequency_table_count = countof(kFtblMclk1ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kMclk2ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kMclk2CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblMclk2ClkSrc,
        .frequency_table_count = countof(kFtblMclk2ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kMclk3ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kMclk3CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblMclk3ClkSrc,
        .frequency_table_count = countof(kFtblMclk3ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi0phytimerClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi0phytimerCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi0phytimerClkSrc,
        .frequency_table_count = countof(kFtblCsi0phytimerClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi1phytimerClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi1phytimerCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi1phytimerClkSrc,
        .frequency_table_count = countof(kFtblCsi1phytimerClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCsi2phytimerClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCsi2phytimerCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCsi2phytimerClkSrc,
        .frequency_table_count = countof(kFtblCsi2phytimerClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kCryptoClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kCryptoCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblCryptoClkSrc,
        .frequency_table_count = countof(kFtblCryptoClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kGp1ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kGp1CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblGp1ClkSrc,
        .frequency_table_count = countof(kFtblGp1ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kGp2ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kGp2CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblGp2ClkSrc,
        .frequency_table_count = countof(kFtblGp2ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kGp3ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kGp3CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblGp3ClkSrc,
        .frequency_table_count = countof(kFtblGp3ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kByte0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kByte0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblByte0ClkSrc,
        .frequency_table_count = countof(kFtblByte0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kByte1ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kByte1CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblByte1ClkSrc,
        .frequency_table_count = countof(kFtblByte1ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kEsc0ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kEsc0CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblEsc0ClkSrc,
        .frequency_table_count = countof(kFtblEsc0ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kEsc1ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kEsc1CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblEsc1ClkSrc,
        .frequency_table_count = countof(kFtblEsc1ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kVsyncClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kVsyncCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblVsyncClkSrc,
        .frequency_table_count = countof(kFtblVsyncClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kPdm2ClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kPdm2CmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblPdm2ClkSrc,
        .frequency_table_count = countof(kFtblPdm2ClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kRbcprGfxClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kRbcprGfxCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::HalfInteger,
        .frequency_table = kFtblRbcprGfxClkSrc,
        .frequency_table_count = countof(kFtblRbcprGfxClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kSdcc1AppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kSdcc1AppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblSdcc1AppsClkSrc,
        .frequency_table_count = countof(kFtblSdcc1AppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kSdcc1IceCoreClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kSdcc1IceCoreCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblSdcc1IceCoreClkSrc,
        .frequency_table_count = countof(kFtblSdcc1IceCoreClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kSdcc2AppsClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kSdcc2AppsCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblSdcc2AppsClkSrc,
        .frequency_table_count = countof(kFtblSdcc2AppsClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kUsb30MockUtmiClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kUsb30MockUtmiCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblUsb30MockUtmiClkSrc,
        .frequency_table_count = countof(kFtblUsb30MockUtmiClkSrc),
    },
    [msm8x53::MsmClkIndex(msm8x53::kUsb3AuxClkSrc)] = {
        .cmd_rcgr_reg = msm8x53::kUsb3AuxCmdRcgr,
        .unsupported = false,
        .type = RcgDividerType::Mnd,
        .frequency_table = kFtblUsb3AuxClkSrc,
        .frequency_table_count = countof(kFtblUsb3AuxClkSrc),
    },
};

static_assert(msm8x53::kRcgClkCount == countof(kMsmClkRcgs),
              "kRcgClkCount must match count of RCG clocks");

constexpr struct msm_clk_voter kMsmClkVoters[] = {
    [msm8x53::MsmClkIndex(msm8x53::kApssAhbClk)] = {
        .cbcr_reg = msm8x53::kApssAhbCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 14)
    },
    [msm8x53::MsmClkIndex(msm8x53::kApssAxiClk)] = {
        .cbcr_reg = msm8x53::kApssAxiCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 13)
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp1AhbClk)] = {
        .cbcr_reg = msm8x53::kBlsp1AhbCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 10)
    },
    [msm8x53::MsmClkIndex(msm8x53::kBlsp2AhbClk)] = {
        .cbcr_reg = msm8x53::kBlsp2AhbCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 20)
    },
    [msm8x53::MsmClkIndex(msm8x53::kBootRomAhbClk)] = {
        .cbcr_reg = msm8x53::kBootRomAhbCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 7)
    },
    [msm8x53::MsmClkIndex(msm8x53::kCryptoAhbClk)] = {
        .cbcr_reg = msm8x53::kCryptoAhbCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 0)
    },
    [msm8x53::MsmClkIndex(msm8x53::kCryptoAxiClk)] = {
        .cbcr_reg = msm8x53::kCryptoAxiCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 1)
    },
    [msm8x53::MsmClkIndex(msm8x53::kCryptoClk)] = {
        .cbcr_reg = msm8x53::kCryptoCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 2)
    },
    [msm8x53::MsmClkIndex(msm8x53::kQdssDapClk)] = {
        .cbcr_reg = msm8x53::kQdssDapCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 11)
    },
    [msm8x53::MsmClkIndex(msm8x53::kPrngAhbClk)] = {
        .cbcr_reg = msm8x53::kPrngAhbCbcr,
        .vote_reg = msm8x53::kApcsClockBranchEnaVote,
        .bit = (1 << 8)
    },
    [msm8x53::MsmClkIndex(msm8x53::kApssTcuAsyncClk)] = {
        .cbcr_reg = msm8x53::kApssTcuAsyncCbcr,
        .vote_reg = msm8x53::kApcsSmmuClockBranchEnaVote,
        .bit = (1 << 1)
    },
    [msm8x53::MsmClkIndex(msm8x53::kCppTbuClk)] = {
        .cbcr_reg = msm8x53::kCppTbuCbcr,
        .vote_reg = msm8x53::kApcsSmmuClockBranchEnaVote,
        .bit = (1 << 14)
    },
    [msm8x53::MsmClkIndex(msm8x53::kJpegTbuClk)] = {
        .cbcr_reg = msm8x53::kJpegTbuCbcr,
        .vote_reg = msm8x53::kApcsSmmuClockBranchEnaVote,
        .bit = (1 << 10)
    },
    [msm8x53::MsmClkIndex(msm8x53::kMdpTbuClk)] = {
        .cbcr_reg = msm8x53::kMdpTbuCbcr,
        .vote_reg = msm8x53::kApcsSmmuClockBranchEnaVote,
        .bit = (1 << 4)
    },
    [msm8x53::MsmClkIndex(msm8x53::kSmmuCfgClk)] = {
        .cbcr_reg = msm8x53::kSmmuCfgCbcr,
        .vote_reg = msm8x53::kApcsSmmuClockBranchEnaVote,
        .bit = (1 << 12)
    },
    [msm8x53::MsmClkIndex(msm8x53::kVenusTbuClk)] = {
        .cbcr_reg = msm8x53::kVenusTbuCbcr,
        .vote_reg = msm8x53::kApcsSmmuClockBranchEnaVote,
        .bit = (1 << 5)
    },
    [msm8x53::MsmClkIndex(msm8x53::kVfe1TbuClk)] = {
        .cbcr_reg = msm8x53::kVfe1TbuCbcr,
        .vote_reg = msm8x53::kApcsSmmuClockBranchEnaVote,
        .bit = (1 << 17)
    },
    [msm8x53::MsmClkIndex(msm8x53::kVfeTbuClk)] = {
        .cbcr_reg = msm8x53::kVfeTbuCbcr,
        .vote_reg = msm8x53::kApcsSmmuClockBranchEnaVote,
        .bit = (1 << 9)
    },
};

void RcgSetRateHalfInteger(const msm_clk_rcg_t& clk, const RcgFrequencyTable* table) {
    #error Implement Me
}

void RcgSetRateMnd(const msm_clk_rcg_t& clk, const RcgFrequencyTable* table) {
    #error Implement Me
}

} // namespace

zx_status_t Msm8x53Clk::Create(void* ctx, zx_device_t* parent) {
    zx_status_t status;

    std::unique_ptr<Msm8x53Clk> device(new Msm8x53Clk(parent));

    status = device->Init();
    if (status != ZX_OK) {
        zxlogf(ERROR, "msm-clk: failed to initialize, st = %d\n", status);
        return status;
    }

    status = device->DdkAdd(kMsmClkName);
    if (status != ZX_OK) {
        zxlogf(ERROR, "msm-clk: DdkAdd failed, st = %d\n", status);
        return status;
    }

    // Intentially leak, devmgr owns the memory now.
    __UNUSED auto* unused = device.release();

    return ZX_OK;
}

zx_status_t Msm8x53Clk::Init() {
    ddk::PDev pdev(parent());
    if (!pdev.is_valid()) {
        zxlogf(ERROR, "msm-clk: failed to get pdev protocol\n");
        return ZX_ERR_NO_RESOURCES;
    }

    fbl::AutoLock lock(&rcg_rates_lock_);
    for (size_t i = 0; i < msm8x53::kRcgClkCount; i++) {
        rcg_rates_[i] = kRcgRateUnset;
    }

    zx_status_t status = pdev.MapMmio(0, &mmio_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "msm-clk: failed to map cc_base mmio, st = %d\n", status);
        return status;
    }

    status = RegisterClockProtocol();
    if (status != ZX_OK) {
        zxlogf(ERROR, "msm-clk: failed to register clock impl protocol, st = %d\n", status);
        return status;
    }

    return ZX_OK;
}

zx_status_t Msm8x53Clk::ClockImplEnable(uint32_t index) {
    // Extract the index and the type of the clock from the argument.
    const uint32_t clock_id = msm8x53::MsmClkIndex(index);
    const msm8x53::msm_clk_type clock_type = msm8x53::MsmClkType(index);

    switch (clock_type) {
    case msm8x53::msm_clk_type::kGate:
        return GateClockEnable(clock_id);
    case msm8x53::msm_clk_type::kBranch:
        return BranchClockEnable(clock_id);
    case msm8x53::msm_clk_type::kVoter:
        return VoterClockEnable(clock_id);
    case msm8x53::msm_clk_type::kRcg:
        return RcgClockEnable(clock_id);
    }

    // Unimplemented clock type?
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Msm8x53Clk::ClockImplDisable(uint32_t index) {
    // Extract the index and the type of the clock from the argument.
    const uint32_t clock_id = msm8x53::MsmClkIndex(index);
    const msm8x53::msm_clk_type clock_type = msm8x53::MsmClkType(index);

    switch (clock_type) {
    case msm8x53::msm_clk_type::kGate:
        return GateClockDisable(clock_id);
    case msm8x53::msm_clk_type::kBranch:
        return BranchClockDisable(clock_id);
    case msm8x53::msm_clk_type::kVoter:
        return VoterClockDisable(clock_id);
    case msm8x53::msm_clk_type::kRcg:
        return RcgClockDisable(clock_id);
    }

    // Unimplemented clock type?
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Msm8x53Clk::ClockImplRequestRate(uint32_t id, uint64_t hz) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Msm8x53Clk::AwaitBranchClock(Toggle status, const uint32_t cbcr_reg) {
    // In case the status check register and the clock control register cross
    // a boundary.
    hw_mb();

    // clang-format off
    constexpr uint32_t kReadyMask             = 0xf0000000;
    constexpr uint32_t kBranchEnableVal       = 0x0;
    constexpr uint32_t kBranchDisableVal      = 0x80000000;
    constexpr uint32_t kBranchNocFsmEnableVal = 0x20000000;
    // clang-format on

    constexpr uint32_t kMaxAttempts = 500;
    for (uint32_t attempts = 0; attempts < kMaxAttempts; attempts++) {
        const uint32_t val = mmio_->Read32(cbcr_reg) & kReadyMask;

        switch (status) {
        case Toggle::Enabled:
            if ((val == kBranchEnableVal) || (val == kBranchNocFsmEnableVal)) {
                return ZX_OK;
            }
            break;
        case Toggle::Disabled:
            if (val == kBranchDisableVal) {
                return ZX_OK;
            }
            break;
        }

        zx_nanosleep(zx_deadline_after(ZX_USEC(1)));
    }

    return ZX_ERR_TIMED_OUT;
}

zx_status_t Msm8x53Clk::VoterClockEnable(uint32_t index) {
    if (unlikely(index >= countof(kMsmClkVoters))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const struct clk::msm_clk_voter& clk = kMsmClkVoters[index];

    lock_.Acquire();
    mmio_->SetBits32(clk.bit, clk.vote_reg);
    lock_.Release();

    return AwaitBranchClock(Toggle::Enabled, clk.cbcr_reg);
}

zx_status_t Msm8x53Clk::VoterClockDisable(uint32_t index) {
    if (unlikely(index >= countof(kMsmClkVoters))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const struct clk::msm_clk_voter& clk = kMsmClkVoters[index];

    lock_.Acquire();
    mmio_->ClearBits32(clk.bit, clk.vote_reg);
    lock_.Release();

    return ZX_OK;
}

zx_status_t Msm8x53Clk::BranchClockEnable(uint32_t index) {
    if (unlikely(index >= countof(kMsmClkBranches))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const struct clk::msm_clk_branch& clk = kMsmClkBranches[index];

    lock_.Acquire();
    mmio_->SetBits32(kBranchEnable, clk.reg);
    lock_.Release();

    return AwaitBranchClock(Toggle::Enabled, clk.reg);
}

zx_status_t Msm8x53Clk::BranchClockDisable(uint32_t index) {
    if (unlikely(index >= countof(kMsmClkBranches))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const struct msm_clk_branch& clk = kMsmClkBranches[index];

    lock_.Acquire();
    mmio_->ClearBits32(kBranchEnable, clk.reg);
    lock_.Release();

    return AwaitBranchClock(Toggle::Disabled, clk.reg);
}

zx_status_t Msm8x53Clk::GateClockEnable(uint32_t index) {
    if (unlikely(index >= countof(kMsmClkGates))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const msm_clk_gate_t& clk = kMsmClkGates[index];

    lock_.Acquire();
    mmio_->SetBits32(clk.bit, clk.reg);
    lock_.Release();

    if (clk.delay_us) {
        zx_nanosleep(zx_deadline_after(ZX_USEC(clk.delay_us)));
    }

    return ZX_OK;
}
zx_status_t Msm8x53Clk::GateClockDisable(uint32_t index) {
    if (unlikely(index > countof(kMsmClkGates))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const msm_clk_gate_t& clk = kMsmClkGates[index];

    lock_.Acquire();
    mmio_->ClearBits32(clk.bit, clk.reg);
    lock_.Release();

    if (clk.delay_us) {
        zx_nanosleep(zx_deadline_after(ZX_USEC(clk.delay_us)));
    }

    return ZX_OK;
}

zx_status_t Msm8x53Clk::RcgClockEnable(uint32_t index) {
    if (unlikely(index > countof(kMsmClkRcgs))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const msm_clk_rcg_t& clk = kMsmClkRcgs[index];

    // Check to see if frequency has been set.
    fbl::AutoLock lock(&rcg_rates_lock_);
    if (rcg_rates_[index] == kRcgRateUnset) {
        zxlogf(ERROR, "Attempted to enable RCG %u before setting rate\n", index);
        return ZX_ERR_BAD_STATE;
    }

    zx_status_t st;

    st = ToggleRcgForceEnable(clk.cmd_rcgr_reg, Toggle::Enabled);
    if (st != ZX_OK) {
        return st;
    }

    st = RcgClockSetRate(index, rcg_rates_[index]);
    if (st != ZX_OK) {
        return st;
    }

    st = ToggleRcgForceEnable(clk.cmd_rcgr_reg, Toggle::Disabled);
    if (st != ZX_OK) {
        return st;
    }

    return st;
}

zx_status_t Msm8x53Clk::RcgClockSetRate(uint32_t index, uint64_t rate) {
    if (unlikely(index > countof(kMsmClkRcgs))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const msm_clk_rcg_t& clk = kMsmClkRcgs[index];

    // Clocks with non-local children or nonlocal control timeouts are
    // currently unimplemented.
    // Clocks with source frequencies that are not fixed are also currently
    // unimplemented.
    if (clk.unsupported) {
        zxlogf(ERROR, "Attempted to set rate for clock %u which is currently "
               "unimplemented\n", index);
        return ZX_ERR_NOT_SUPPORTED;
    }

    // Search for the requested frequency in the clock's frequency table.
    const RcgFrequencyTable* table = nullptr;
    for (size_t i = 0; i < clk.frequency_table_count; i++) {
        if (rate == clk.frequency_table[i].rate()) {
            table = &clk.frequency_table[i];
            break;
        }
    }

    if (table == nullptr) {
        // This clock frequency is not supported.
        zxlogf(WARN, "unsupported clock frequency\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    switch (clk.type) {
        case RcgDividerType::HalfInteger:
            RcgSetRateHalfInteger(clk, table);
            break;
        case RcgDividerType::Mnd:
            RcgSetRateMnd(clk, table);
            break;
    }

    // Update the frequency that we have listed in the RCG table.

    return ZX_OK;
}

zx_status_t Msm8x53Clk::RcgClockDisable(uint32_t index) {

    return ZX_OK;
}

zx_status_t Msm8x53Clk::ToggleRcgForceEnable(uint32_t rcgr_cmd_offset, Toggle toggle) {
    constexpr uint32_t kRcgForceDisableDelayUSeconds = 100;
    constexpr uint32_t kRcgRootEnableBit = (1 << 1);
    zx_status_t result = ZX_OK;

    switch (toggle) {
    case Toggle::Enabled:
        lock_.Acquire();
        mmio_->SetBits32(kRcgRootEnableBit, rcgr_cmd_offset);
        result = AwaitRcgEnableLocked(rcgr_cmd_offset);
        lock_.Release();
        break;
    case Toggle::Disabled:
        lock_.Acquire();
        mmio_->ClearBits32(kRcgRootEnableBit, rcgr_cmd_offset);
        lock_.Release();
        zx_nanosleep(zx_deadline_after(ZX_USEC(kRcgForceDisableDelayUSeconds)));
        break;
    }
    return result;
}

zx_status_t Msm8x53Clk::AwaitRcgEnableLocked(uint32_t rcgr_cmd_offset) {

    for (uint32_t i = 0; i < kRcgUpdateTimeoutUsec; i++) {
        auto rcg_ctrl = RcgClkCmd::Read(rcgr_cmd_offset).ReadFrom(&(*mmio_));

        if (rcg_ctrl.root_status() == 0) {
            return ZX_OK;
        }

        zx_nanosleep(zx_deadline_after(ZX_USEC(1)));
    }

    return ZX_ERR_TIMED_OUT;
}

zx_status_t Msm8x53Clk::Bind() {
    return ZX_OK;
}
void Msm8x53Clk::DdkUnbind() {
    fbl::AutoLock lock(&lock_);

    mmio_.reset();

    DdkRemove();
}

void Msm8x53Clk::DdkRelease() {
    delete this;
}

zx_status_t Msm8x53Clk::RegisterClockProtocol() {
    zx_status_t st;

    ddk::PBusProtocolClient pbus(parent());
    if (!pbus.is_valid()) {
        return ZX_ERR_NO_RESOURCES;
    }

    clock_impl_protocol_t clk_proto = {
        .ops = &clock_impl_protocol_ops_,
        .ctx = this,
    };

    st = pbus.RegisterProtocol(ZX_PROTOCOL_CLOCK_IMPL, &clk_proto, sizeof(clk_proto));
    if (st != ZX_OK) {
        zxlogf(ERROR, "msm-clk: pbus_register_protocol failed, st = %d\n", st);
        return st;
    }

    return ZX_OK;
}

} // namespace clk

static zx_driver_ops_t msm8x53_clk_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = clk::Msm8x53Clk::Create;
    return ops;
}();

// clang-format off
ZIRCON_DRIVER_BEGIN(msm8x53_clk, msm8x53_clk_driver_ops, "zircon", "0.1", 3)
BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_QUALCOMM),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_QUALCOMM_CLOCK),
ZIRCON_DRIVER_END(msm8x53_clk)
