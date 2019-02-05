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

#define HI32(val) ((uint32_t)(((val) >> 32) & 0xfffffffflu))
#define LO32(val) ((uint32_t)((val)&0xfffffffflu))

class HisiPcieDevice {
public:
    explicit HisiPcieDevice(zx_device_t* device)
        : parent_(device) {}
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

    void dw_pcie_prog_outbound_atu(int index, int type, uint64_t cpu_addr,
                                   uint64_t pci_addr, uint32_t size);

    zx_status_t zx_init_pcie_driver();

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

    std::optional<ddk::MmioBuffer> config_;
};

#define TOTAL_APP_SIZE (0x02000000)     // 32MiB
#define CFG_ADDR_LEN   (0x100000)       // 1024KiB
#define MEM_ADDR_BASE  (0xf6000000)
#define MEM_ADDR_LEN   (TOTAL_APP_SIZE - CFG_ADDR_LEN)
#define CFG_ADDR_BASE  (MEM_ADDR_BASE + MEM_ADDR_LEN)


uint32_t HisiPcieDevice::kirin_phy_readl(uint32_t reg) {
    return phy_->Read32(reg);
}

void HisiPcieDevice::kirin_phy_writel(uint32_t val, uint32_t reg) {
    phy_->Write32(val, reg);
}

#define PCIE_ATU_VIEWPORT 0x900
#define PCIE_ATU_REGION_INBOUND (0x1 << 31)
#define PCIE_ATU_REGION_OUTBOUND (0x0 << 31)
#define PCIE_ATU_REGION_INDEX2 (0x2 << 0)
#define PCIE_ATU_REGION_INDEX1 (0x1 << 0)
#define PCIE_ATU_REGION_INDEX0 (0x0 << 0)
#define PCIE_ATU_CR1 0x904
#define PCIE_ATU_TYPE_MEM (0x0 << 0)
#define PCIE_ATU_TYPE_IO (0x2 << 0)
#define PCIE_ATU_TYPE_CFG0 (0x4 << 0)
#define PCIE_ATU_TYPE_CFG1 (0x5 << 0)
#define PCIE_ATU_CR2 0x908
#define PCIE_ATU_ENABLE ((0x1ul << 31))
#define PCIE_ATU_BAR_MODE_ENABLE (0x1 << 30)
#define PCIE_ATU_LOWER_BASE 0x90C
#define PCIE_ATU_UPPER_BASE 0x910
#define PCIE_ATU_LIMIT 0x914
#define PCIE_ATU_LOWER_TARGET 0x918
#define PCIE_ATU_BUS(x) (((x)&0xff) << 24)
#define PCIE_ATU_DEV(x) (((x)&0x1f) << 19)
#define PCIE_ATU_FUNC(x) (((x)&0x7) << 16)
#define PCIE_ATU_UPPER_TARGET 0x91C
void HisiPcieDevice::dw_pcie_prog_outbound_atu(int index, int type, uint64_t cpu_addr,
                                               uint64_t pci_addr, uint32_t size) {
    kirin_pcie_writel_rc(PCIE_ATU_VIEWPORT, PCIE_ATU_REGION_OUTBOUND | index);
    kirin_pcie_writel_rc(PCIE_ATU_LOWER_BASE, LO32(cpu_addr));
    kirin_pcie_writel_rc(PCIE_ATU_UPPER_BASE, HI32(cpu_addr));
    kirin_pcie_writel_rc(PCIE_ATU_LIMIT, LO32(cpu_addr + size - 1));
    kirin_pcie_writel_rc(PCIE_ATU_LOWER_TARGET, LO32(pci_addr));
    kirin_pcie_writel_rc(PCIE_ATU_UPPER_TARGET, HI32(pci_addr));
    kirin_pcie_writel_rc(PCIE_ATU_CR1, type);
    kirin_pcie_writel_rc(PCIE_ATU_CR2, PCIE_ATU_ENABLE);

    for (int attempts = 0; attempts < 5; attempts++) {
        uint32_t val = kirin_pcie_readl_rc(PCIE_ATU_CR2);
        if (val == PCIE_ATU_ENABLE)
            return;

        zx_nanosleep(zx_deadline_after(ZX_USEC(10000)));
    }

    zxlogf(ERROR, "failed to program outbound atu");
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

#define SOC_PCIECTRL_CTRL0_ADDR (0x000)
#define SOC_PCIECTRL_CTRL1_ADDR (0x004)
#define PCIE_ELBI_SLV_DBI_ENABLE (0x1 << 21)

void HisiPcieDevice::kirin_pcie_sideband_dbi_r_mode(bool enable) {
    return;
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
    return;
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
#define PCIE_ELBI_RDLH_LINKUP (0x400)
#define PCIE_LINKUP_ENABLE (0x8020)
    uint32_t val = kirin_elb_readl(PCIE_ELBI_RDLH_LINKUP);

    return (val & PCIE_LINKUP_ENABLE) == PCIE_LINKUP_ENABLE;
}

#define PCIE_PORT_LINK_CONTROL (0x710)
#define PORT_LINK_MODE_MASK (0x3f << 16)
#define PORT_LINK_MODE_1_LANES (0x1 << 16)
#define PCIE_LINK_WIDTH_SPEED_CONTROL (0x80C)

#define PORT_LOGIC_LINK_WIDTH_1_LANES (0x1 << 8)
#define PORT_LOGIC_SPEED_CHANGE (0x1 << 17)
#define PORT_LOGIC_LINK_WIDTH_MASK (0x1f << 8)

#define  PCI_COMMAND_IO     0x1 /* Enable response in I/O space */
#define  PCI_COMMAND_MEMORY 0x2 /* Enable response in Memory space */
#define  PCI_COMMAND_MASTER 0x4 /* Enable bus mastering */
#define  PCI_COMMAND_SPECIAL    0x8 /* Enable response to special cycles */
#define  PCI_COMMAND_INVALIDATE 0x10    /* Use memory write and invalidate */
#define  PCI_COMMAND_VGA_PALETTE 0x20   /* Enable palette snooping */
#define  PCI_COMMAND_PARITY 0x40    /* Enable parity checking */
#define  PCI_COMMAND_WAIT   0x80    /* Enable address/data stepping */
#define  PCI_COMMAND_SERR   0x100   /* Enable SERR */
#define  PCI_COMMAND_FAST_BACK  0x200   /* Enable back-to-back writes */
#define  PCI_COMMAND_INTX_DISABLE 0x400 /* INTx Emulation Disable */

#define PF_MEM_LEN (24 * 1024 * 1024)
#define MEM_LEN    (MEM_ADDR_LEN - PF_MEM_LEN)

static_assert(PF_MEM_LEN < MEM_ADDR_LEN);
static_assert(MEM_LEN < MEM_ADDR_LEN);

uint32_t getBaseLimitReg(uint32_t base, uint32_t len) {
    uint32_t result = 0;

    result |= ((base >> 16) & 0xFFF0);
    result |= ((base + len - 1) & 0xFFF00000);

    return result;
}

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

    // RC Bars
    // kirin_pcie_writel_rc(0x10, 0x00000004);
    // kirin_pcie_writel_rc(0x14, 0x00000000);

    // Interrupt pins
    val = kirin_pcie_readl_rc(0x3c);
    val &= 0xffff00ff;
    val |= 0x00000100;
    kirin_pcie_writel_rc(0x3c, val);

    // Setup bus numbers
    val = kirin_pcie_readl_rc(0x18);
    val &= 0xff000000;
    val |= 0x00ff0100;
    kirin_pcie_writel_rc(0x18, val);

    // Setup memory base and limit registers.
    kirin_pcie_rd_own_conf(0x20, 4, &val);
    // Memory base  = 0xf610.0000
    // Memory Limit = 0xf80f.ffff
    // val = 0xf610f800;
    // val = 0xf7f0f610;
    // val = 0xf700f610;
    val = getBaseLimitReg(MEM_ADDR_BASE + PF_MEM_LEN, MEM_LEN);
    kirin_pcie_wr_own_conf(0x20, 4, val);

        // Setup memory base and limit registers.
    kirin_pcie_rd_own_conf(0x24, 4, &val);
    // Memory base  = 0xf610.0000
    // Memory Limit = 0xf80f.ffff
    // val = 0xf610f800;
    // val = 0xf7f0f710;
    // val = getBaseLimitReg(MEM_ADDR_BASE, PF_MEM_LEN);
    val = 0xF000FFFF;
    kirin_pcie_wr_own_conf(0x24, 4, val);

    val = 0;
    kirin_pcie_wr_own_conf(0x28, 4, val);
    kirin_pcie_wr_own_conf(0x2C, 4, val);


    kirin_pcie_rd_own_conf(0x20, 4, &val);
    printf("READ mem prefetch = 0x%08x\n", val);

    // Command register
    val = kirin_pcie_readl_rc(0x04);
    val &= 0xffff0000;
    val |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
           PCI_COMMAND_MASTER | PCI_COMMAND_SERR;
    kirin_pcie_writel_rc(0x04, val);

    // Zero out the IO/Limit and IO Base registers
    // kirin_pcie_rd_own_conf(0x1C, 4, &val);
    // val &= 0xFFFF0000;
    // val = 0;
    // kirin_pcie_wr_own_conf(0x1C, 4, val);
    val = kirin_pcie_readl_rc(0x1C);
    // val &= 0xFFFF0000;
    val = 0xF0;
    kirin_pcie_writel_rc(0x1C, val);

    kirin_pcie_rd_own_conf(0x8, 4, &val);
    val &= 0x000000FF;
    val |= 0x00040600;
    kirin_pcie_wr_own_conf(0x8, 4, val);

}

#define PCIE_LTSSM_ENABLE_BIT (0x1 << 11)
#define PCIE_APP_LTSSM_ENABLE 0x01c
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

#define MMIO_DBI 0
#define MMIO_APB 1
#define MMIO_PHY 2
#define MMIO_CFG 3
#define MMIO_CRGCTRL 4
#define MMIO_SCTRL 5
#define MMIO_CONFIG 6

zx_status_t HisiPcieDevice::InitMmios() {
    zx_status_t st;
    mmio_buffer_t mmio;

    st = pdev_map_mmio_buffer(&pdev_, MMIO_DBI, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map dbi buffer, st = %d\n", st);
        return st;
    }
    dbi_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer(&pdev_, MMIO_APB, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map dbi buffer, st = %d\n", st);
        return st;
    }
    apb_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer(&pdev_, MMIO_PHY, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map dbi buffer, st = %d\n", st);
        return st;
    }
    phy_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer(&pdev_, MMIO_CFG, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map dbi buffer, st = %d\n", st);
        return st;
    }
    cfg_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer(&pdev_, MMIO_CRGCTRL, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map crgctrl buffer, st = %d\n", st);
        return st;
    }
    crgctrl_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer(&pdev_, MMIO_SCTRL, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map sctrl buffer, st = %d\n", st);
        return st;
    }
    sctrl_ = ddk::MmioBuffer(mmio);

    st = pdev_map_mmio_buffer(&pdev_, MMIO_CONFIG, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to map config buffer, st = %d\n", st);
        return st;
    }
    config_ = ddk::MmioBuffer(mmio);

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

    uint32_t val;
    for (uint32_t i = 0; i < 256; i += 4) {
        kirin_pcie_rd_own_conf(i, 4, &val);
        printf("hisi_pcie: rd own conf %xh = 0x%08x\n", i, val);
    }

    // BAR requirements for the root bridge.
    // kirin_pcie_wr_own_conf(0x10, 4, 0xffffffff);
    // val = kirin_pcie_rd_own_conf(0x10, 4, &val);
    // printf("Bridge BAR0 = 0x%08x\n", val);

    // kirin_pcie_wr_own_conf(0x14, 4, 0xffffffff);
    // val = kirin_pcie_rd_own_conf(0x14, 4, &val);
    // printf("Bridge BAR1 = 0x%08x\n", val);

    return zx_init_pcie_driver();
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

#define HI3660_CLK_GATE_PCIEAUX 0
#define HI3660_PCLK_GATE_PCIE_PHY 1
#define HI3660_PCLK_GATE_PCIE_SYS 2
#define HI3660_ACLK_GATE_PCIE 3

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


// #define PIO_ADDR_LEN   (0x100000)       // 4KiB
// #define MEM_ADDR_LEN   (TOTAL_APP_SIZE - CFG_ADDR_LEN)

// #define CFG_ADDR_BASE  (0xf6000000)
// #define PIO_ADDR_BASE  (CFG_ADDR_BASE + CFG_ADDR_LEN)
// #define MEM_ADDR_BASE  (CFG_ADDR_BASE + CFG_ADDR_LEN)


zx_status_t HisiPcieDevice::zx_init_pcie_driver() {
    zx_status_t st;

    dw_pcie_prog_outbound_atu(PCIE_ATU_REGION_INDEX0, PCIE_ATU_TYPE_MEM,
                              MEM_ADDR_BASE, MEM_ADDR_BASE, MEM_ADDR_LEN);

    dw_pcie_prog_outbound_atu(PCIE_ATU_REGION_INDEX1, PCIE_ATU_TYPE_CFG0,
                              CFG_ADDR_BASE, CFG_ADDR_BASE, CFG_ADDR_LEN);

    // dw_pcie_prog_outbound_atu(PCIE_ATU_REGION_INDEX2, PCIE_ATU_TYPE_IO,
    //                           PIO_ADDR_BASE, 0x0, PIO_ADDR_LEN);

    // Program ATU regions and add/sub IO ranges
    st = zx_pci_add_subtract_io_range(
        get_root_resource(),
        true,
        MEM_ADDR_BASE,
        MEM_ADDR_LEN,
        true
    );
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to add pcie mmio range, st = %d\n", st);
        return st;
    }

    // Program ATU regions and add/sub IO ranges
    // st = zx_pci_add_subtract_io_range(
    //     get_root_resource(),
    //     false,
    //     PIO_ADDR_BASE,
    //     PIO_ADDR_LEN,
    //     true
    // );
    // if (st != ZX_OK) {
    //     zxlogf(ERROR, "hisi_pcie: failed to add pcie mmio range, st = %d\n", st);
    //     return st;
    // }

    zx_pci_init_arg_t* arg;
    const size_t arg_size = sizeof(*arg) + sizeof(arg->addr_windows[0]) * 2;
    arg = (zx_pci_init_arg_t*)calloc(1, arg_size);

    arg->num_irqs = 0;
    arg->addr_window_count = 2;

    // Root Bridge Config Window.
    arg->addr_windows[0].cfg_space_type = PCI_CFG_SPACE_TYPE_DW_ROOT;
    arg->addr_windows[0].has_ecam = true;
    arg->addr_windows[0].base = 0xf4000000;
    arg->addr_windows[0].size = 0x1000;
    arg->addr_windows[0].bus_start = 0;
    arg->addr_windows[0].bus_end = 0;

    // Downstream Config Window.
    arg->addr_windows[1].cfg_space_type = PCI_CFG_SPACE_TYPE_DW_DS;
    arg->addr_windows[1].has_ecam = true;
    arg->addr_windows[1].base = CFG_ADDR_BASE;
    arg->addr_windows[1].size = CFG_ADDR_LEN;
    arg->addr_windows[1].bus_start = 1;
    arg->addr_windows[1].bus_end = 1;

    st = zx_pci_init(get_root_resource(), arg, arg_size);
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to init pci bus driver, st = %d\n", st);
        return st;
    }

    return ZX_OK;
}

extern zx_status_t hisi_pcie_bind(void* ctx, zx_device_t* device) {
    HisiPcieDevice* dev = new HisiPcieDevice(device);

    zx_status_t st = dev->Init();
    if (st != ZX_OK) {
        zxlogf(ERROR, "hisi_pcie: failed to start, st = %d\n", st);
        delete dev;
    } else {
        printf("hisi_pcie: dev->Init() success!\n");
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