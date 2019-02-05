// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Temporary for debugging.
#include <stdio.h>

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/clk.h>
#include <ddk/protocol/gpio.h>
#include <ddk/protocol/platform-device-lib.h>
#include <ddk/protocol/platform/device.h>
#include <ddktl/mmio.h>

#include <zircon/driver/binding.h>
#include <zircon/types.h>

class HisiPcieDevice {
  public:
    explicit HisiPcieDevice(zx_device_t* device) : parent_(device) {}
    ~HisiPcieDevice() {}
    zx_status_t Init();
  private:
    zx_status_t InitProtocols();
    zx_status_t InitMmios();

    zx_status_t kirin_pcie_get_clks();
    zx_status_t kirin_pcie_get_resource();
    zx_status_t kirin_pcie_configure_gpios();
    zx_status_t kirin_pcie_power_on();
    zx_status_t kirin_add_pcie_port();
    void kirin_pcie_oe_enable();
    zx_status_t kirin_pcie_clk_ctrl(const bool enable);
    zx_status_t kirin_pcie_phy_init();
    zx_status_t dw_pcie_host_init();

    zx_status_t kirin_pcie_establish_link();
    void kirin_pcie_host_init();

    bool kirin_pcie_link_up();

    uint32_t kirin_phy_readl(uint32_t reg);
    void kirin_phy_writel(uint32_t val, uint32_t reg);

    uint32_t kirin_pcie_readl_rc(uint32_t reg);
    void kirin_pcie_writel_rc(uint32_t reg, uint32_t val);

    void kirin_pcie_sideband_dbi_r_mode(bool enable);
    void kirin_pcie_sideband_dbi_w_mode(bool enable);

    uint32_t kirin_elb_readl(uint32_t reg);
    void kirin_elb_writel(uint32_t val, uint32_t reg);

    int kirin_pcie_rd_own_conf(uint32_t where, int size, uint32_t* val);
    int kirin_pcie_wr_own_conf(uint32_t where, int size, uint32_t val);

    void dw_pcie_setup_rc();

    zx_device_t* parent_;
    zx_device_t* dev_;

    pdev_protocol_t pdev_;
    clk_protocol_t clk_;
    gpio_protocol_t gpio_;

    std::optional<ddk::MmioBuffer> dbi_;
    std::optional<ddk::MmioBuffer> apb_;
    std::optional<ddk::MmioBuffer> phy_;
    std::optional<ddk::MmioBuffer> cfg_;

    std::optional<ddk::MmioBuffer> crgctrl_;
    std::optional<ddk::MmioBuffer> sctrl_;
};

uint32_t HisiPcieDevice::kirin_phy_readl(uint32_t reg) {
    return phy_->Read32(reg);
}

void HisiPcieDevice::kirin_phy_writel(uint32_t val, uint32_t reg) {
    phy_->Write32(val, reg);
}

uint32_t HisiPcieDevice::kirin_elb_readl(uint32_t reg) {
    return apb_->Read32(reg);
}

void HisiPcieDevice::kirin_elb_writel(uint32_t val, uint32_t reg) {
    return apb_->Write32(val, reg);
}

int HisiPcieDevice::kirin_pcie_rd_own_conf(uint32_t where, int size, uint32_t* val) {
    kirin_pcie_sideband_dbi_r_mode(true);

    *val = dbi_->Read32(where & ~0x3);
    if (size != 4) {
        ZX_PANIC("unimplemented");
    }

    kirin_pcie_sideband_dbi_r_mode(false);

    return 0;
}

int HisiPcieDevice::kirin_pcie_wr_own_conf(uint32_t where, int size, uint32_t val) {
    kirin_pcie_sideband_dbi_w_mode(true);

    if (size == 4) {
        dbi_->Write32(val, where & ~0x3u);
    } else {
        ZX_PANIC("unimplemented");
    }

    kirin_pcie_sideband_dbi_w_mode(false);

    return 0;
}


zx_status_t HisiPcieDevice::InitProtocols() {
    zx_status_t st;

    st = device_get_protocol(parent_, ZX_PROTOCOL_PDEV, &pdev_);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to get pdev, st = %d\n", st);
        return st;
    }

    st = device_get_protocol(parent_, ZX_PROTOCOL_GPIO, &gpio_);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to get gpio, st = %d\n", st);
        return st;
    }

    st = gpio_config_out(&gpio_, 0);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to configure rst gpio, st = %d\n", st);
        return st;
    }

    st = device_get_protocol(parent_, ZX_PROTOCOL_CLK, &clk_);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to get clk protocol, st = %d\n", st);
        return st;
    }

    return st;
}

#define SOC_PCIECTRL_CTRL0_ADDR     (0x000)
#define SOC_PCIECTRL_CTRL1_ADDR     (0x004)
#define PCIE_ELBI_SLV_DBI_ENABLE    (0x1 << 21)

void HisiPcieDevice::kirin_pcie_sideband_dbi_r_mode(bool enable) {
    uint32_t val;

    val = kirin_elb_readl(SOC_PCIECTRL_CTRL1_ADDR);

    if (enable) {
        val |= PCIE_ELBI_SLV_DBI_ENABLE;
    } else {
        val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
    }

    kirin_elb_writel(val, SOC_PCIECTRL_CTRL1_ADDR);
}

void HisiPcieDevice::kirin_pcie_sideband_dbi_w_mode(bool enable) {
    uint32_t val;

    val = kirin_elb_readl(SOC_PCIECTRL_CTRL0_ADDR);

    if (enable) {
        val |= PCIE_ELBI_SLV_DBI_ENABLE;
    } else {
        val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
    }

    kirin_elb_writel(val, SOC_PCIECTRL_CTRL0_ADDR);
}


void HisiPcieDevice::kirin_pcie_host_init() {
    zx_status_t st;

    st = kirin_pcie_establish_link();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: kirin_pcie_host_init failed, st = %d\n", st);
        return;
    }

    printf("LINK ESTABLISHED!\n");

    // TODO(gkalsi): kirin_pcie_enable_interrupts.
}

bool HisiPcieDevice::kirin_pcie_link_up() {
    #define PCIE_ELBI_RDLH_LINKUP       (0x400)
    #define PCIE_LINKUP_ENABLE          (0x8020)
    uint32_t val = kirin_elb_readl(PCIE_ELBI_RDLH_LINKUP);

    return (val & PCIE_LINKUP_ENABLE) == PCIE_LINKUP_ENABLE;
}

#define PCIE_PORT_LINK_CONTROL          (0x710)
#define PORT_LINK_MODE_MASK             (0x3f << 16)
#define PORT_LINK_MODE_1_LANES          (0x1 << 16)
#define PCIE_LINK_WIDTH_SPEED_CONTROL   (0x80C)

#define PORT_LOGIC_LINK_WIDTH_1_LANES   (0x1 << 8)
#define PORT_LOGIC_SPEED_CHANGE     (0x1 << 17)
#define PORT_LOGIC_LINK_WIDTH_MASK  (0x1f << 8)
void HisiPcieDevice::dw_pcie_setup_rc() {
    uint32_t val;

    // Set the number of lanes
    val = kirin_pcie_readl_rc(PCIE_PORT_LINK_CONTROL);
    val &= ~PORT_LINK_MODE_MASK;
    val |= PORT_LINK_MODE_1_LANES;
    kirin_pcie_writel_rc(PCIE_PORT_LINK_CONTROL, val);


    // Set the link width speed.
    val = kirin_pcie_readl_rc(PCIE_LINK_WIDTH_SPEED_CONTROL);
    val &= ~PORT_LOGIC_LINK_WIDTH_MASK;
    val |= PORT_LOGIC_LINK_WIDTH_1_LANES;
    kirin_pcie_writel_rc(PCIE_LINK_WIDTH_SPEED_CONTROL, val);

    kirin_pcie_rd_own_conf(PCIE_LINK_WIDTH_SPEED_CONTROL, 4, &val);
    val |= PORT_LOGIC_SPEED_CHANGE;
    kirin_pcie_wr_own_conf(PCIE_LINK_WIDTH_SPEED_CONTROL, 4, val);
}

#define PCIE_LTSSM_ENABLE_BIT     (0x1 << 11)
#define PCIE_APP_LTSSM_ENABLE       0x01c
zx_status_t HisiPcieDevice::kirin_pcie_establish_link() {
    if (kirin_pcie_link_up()) {
        return ZX_OK;
    }

    dw_pcie_setup_rc();

    // TODO(gkalsi): Assert LTSSM enable
    kirin_elb_writel(PCIE_LTSSM_ENABLE_BIT, PCIE_APP_LTSSM_ENABLE);


    for (uint32_t i = 0; i < 1000; i++) {
        if (kirin_pcie_link_up()) {
            return ZX_OK;
        }

        zx_nanosleep(zx_deadline_after(ZX_MSEC(1)));
    }

    zxlogf(ERROR, "hisi_pcie: link establishment timed out\n");
    return ZX_ERR_TIMED_OUT;
}

#define MMIO_DBI     0
#define MMIO_APB     1
#define MMIO_PHY     2
#define MMIO_CFG     3
#define MMIO_CRGCTRL 4
#define MMIO_SCTRL   5

zx_status_t HisiPcieDevice::InitMmios() {
    zx_status_t st;
    mmio_buffer_t mmio;

    st = pdev_map_mmio_buffer2(&pdev_, MMIO_DBI, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map dbi buffer, st = %d\n", st);
        return st;
    }
    dbi_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer2(&pdev_, MMIO_APB, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map dbi buffer, st = %d\n", st);
        return st;
    }
    apb_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer2(&pdev_, MMIO_PHY, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map dbi buffer, st = %d\n", st);
        return st;
    }
    phy_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer2(&pdev_, MMIO_CFG, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map dbi buffer, st = %d\n", st);
        return st;
    }
    cfg_ = ddk::MmioBuffer(mmio);


    st = pdev_map_mmio_buffer2(&pdev_, MMIO_CRGCTRL, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map crgctrl buffer, st = %d\n", st);
        return st;
    }
    crgctrl_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer2(&pdev_, MMIO_SCTRL, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map sctrl buffer, st = %d\n", st);
        return st;
    }
    sctrl_ = ddk::MmioBuffer(mmio);

    return st;
}


zx_status_t HisiPcieDevice::Init() {
    zx_status_t st;

    st = InitProtocols();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to init protocols, st = %d\n", st);
        return st;
    }

    st = InitMmios();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to init mmios, st = %d\n", st);
        return st;
    }

    st = kirin_pcie_get_clks();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to get clks, st = %d\n", st);
        return st;
    }

    st = kirin_pcie_get_resource();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to get resources, st = %d\n", st);
        return st;
    }

    st = kirin_pcie_configure_gpios();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to configure reset gpio, st = %d\n", st);
        return st;
    }

    st = kirin_pcie_power_on();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to power on pcie, st = %d\n", st);
        return st;
    }

    st = kirin_add_pcie_port();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to add port, st = %d\n", st);
        return st;
    }

    printf("hisi_pcie: initialization complete!\n");

    return ZX_OK;
}

zx_status_t HisiPcieDevice::kirin_pcie_get_clks() {
    return ZX_OK;
}

zx_status_t HisiPcieDevice::kirin_pcie_get_resource() {
    return ZX_OK;
}

zx_status_t HisiPcieDevice::kirin_pcie_configure_gpios() {
    return ZX_OK;
}

uint32_t HisiPcieDevice::kirin_pcie_readl_rc(uint32_t reg) {
    uint32_t val;

    kirin_pcie_sideband_dbi_r_mode(true);

    val = dbi_->Read32(reg);

    kirin_pcie_sideband_dbi_r_mode(false);

    return val;
}

void HisiPcieDevice::kirin_pcie_writel_rc(uint32_t reg, uint32_t val) {
    kirin_pcie_sideband_dbi_w_mode(true);

    dbi_->Write32(val, reg);

    kirin_pcie_sideband_dbi_w_mode(false);
}


void HisiPcieDevice::kirin_pcie_oe_enable() {
    uint32_t val;

    val = sctrl_->Read32(0x1a4);
    val |= 0xF0F400;
    val &= ~(0x3 << 28);
    sctrl_->Write32(val, 0x1a4);
}

#define HI3660_CLK_GATE_PCIEAUX     0
#define HI3660_PCLK_GATE_PCIE_PHY   1
#define HI3660_PCLK_GATE_PCIE_SYS   2
#define HI3660_ACLK_GATE_PCIE       3

zx_status_t HisiPcieDevice::kirin_pcie_clk_ctrl(const bool enable) {
    zx_status_t st;
    // TODO(gkalsi): clk_set_rate for PCIE_REF_CLK

    // TODO(gkalsi): prepare enable for PCIE_REF_CLK

    st = clk_enable(&clk_, HI3660_PCLK_GATE_PCIE_SYS);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to enable HI3660_PCLK_GATE_PCIE_SYS, st = %d\n", st);
        return st;
    }

    st = clk_enable(&clk_, HI3660_PCLK_GATE_PCIE_PHY);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to enable HI3660_PCLK_GATE_PCIE_PHY, st = %d\n", st);
        return st;
    }

    st = clk_enable(&clk_, HI3660_ACLK_GATE_PCIE);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to enable HI3660_ACLK_GATE_PCIE, st = %d\n", st);
        return st;
    }

    st = clk_enable(&clk_, HI3660_CLK_GATE_PCIEAUX);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to enable HI3660_CLK_GATE_PCIEAUX, st = %d\n", st);
        return st;
    }

    return ZX_OK;
}

zx_status_t HisiPcieDevice::kirin_pcie_phy_init() {
    uint32_t reg_val;
    uint32_t pipe_clk_stable = 0x1 << 19;
    uint32_t time = 10;

    reg_val = kirin_phy_readl(0x4);
    reg_val &= ~(0x1 << 8);
    kirin_phy_writel(reg_val, 0x4);

    reg_val = kirin_phy_readl(0x0);
    reg_val &= ~(0x1 << 22);
    kirin_phy_writel(reg_val, 0x0);
    zx_nanosleep(zx_deadline_after(ZX_USEC(10)));

    reg_val = kirin_phy_readl(0x4);
    reg_val &= ~(0x1 << 16);
    kirin_phy_writel(reg_val, 0x4);

    reg_val = kirin_phy_readl(0x400);
    while (reg_val & pipe_clk_stable) {
        zx_nanosleep(zx_deadline_after(ZX_USEC(100)));
        if (time == 0) {
            zxlogf(ERROR, "hisi_pcie: pipe clock did not stabilize\n");
            return ZX_ERR_TIMED_OUT;
        }
        time = time - 1;
        reg_val = kirin_phy_readl(0x400);
    }

    printf("pipe clock stabilized successfully!\n");

    return ZX_OK;
}

zx_status_t HisiPcieDevice::kirin_pcie_power_on() {
    zx_status_t st;

    sctrl_->Write32(0x10, 0x60);
    zx_nanosleep(zx_deadline_after(ZX_USEC(100)));
    kirin_pcie_oe_enable();

    // Enable clocks.
    st = kirin_pcie_clk_ctrl(true);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to enable clocks, st = %d\n", st);
        return st;
    }

    sctrl_->Write32(0x30, 0x44);
    crgctrl_->Write32(0x8c000000, 0x88);
    sctrl_->Write32(0x184000, 0x190);

    st = kirin_pcie_phy_init();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to init kirin pcie phy, st = %d\n", st);
        return st;
    }

    zx_nanosleep(zx_deadline_after(ZX_MSEC(20)));

    gpio_write(&gpio_, 1);

    zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));

    return ZX_OK;
}

zx_status_t HisiPcieDevice::dw_pcie_host_init() {
    // TODO(gkalsi): Lots of shit happens here.

    kirin_pcie_host_init();

    return ZX_OK;
}

zx_status_t HisiPcieDevice::kirin_add_pcie_port() {
    // TODO(gkalsi): Setup IRQs

    return dw_pcie_host_init();
}


extern zx_status_t hisi_pcie_bind(void* ctx, zx_device_t* device) {
    HisiPcieDevice* dev = new HisiPcieDevice(device);

    zx_status_t st = dev->Init();
    if (st != ZX_OK) {
        zxlogf(ERROR,"hisi_pcie: failed to start, st = %d\n", st);
        delete dev;
    }

    return st;
}

static zx_driver_ops_t hisi_pcie_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = hisi_pcie_bind;
    return ops;
}();

// clang-format off
// Bind to ANY Amlogic SoC with a DWC PCIe controller.
ZIRCON_DRIVER_BEGIN(hisi_pcie, hisi_pcie_driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_96BOARDS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_DW_PCIE),
ZIRCON_DRIVER_END(hisi_pcie)