// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/resource.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/usb-peripheral-config.h>
#include <hw/reg.h>
#include <zircon/device/usb-peripheral.h>
#include <zircon/hw/usb.h>
#include <zircon/hw/usb/cdc.h>

#include <soc/aml-common/aml-usb-phy.h>
#include <soc/aml-s912/s912-hw.h>

#include <limits.h>
#include <unistd.h>

#include "vim.h"

namespace vim {

#define PREI_USB_PHY_2_REG_BASE 0xd0078020
#define PREI_USB_PHY_3_REG_BASE 0xd0078080

typedef struct u2p_aml_regs {
	volatile uint32_t u2p_r0;
	volatile uint32_t u2p_r1;
	volatile uint32_t u2p_r2;
} u2p_aml_regs_t;

typedef union u2p_r0 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned bypass_sel:1;
		unsigned bypass_dm_en:1;
		unsigned bypass_dp_en:1;
		unsigned txbitstuffenh:1;
		unsigned txbitstuffen:1;
		unsigned dmpulldown:1;
		unsigned dppulldown:1;
		unsigned vbusvldextsel:1;
		unsigned vbusvldext:1;
		unsigned adp_prb_en:1;
		unsigned adp_dischrg:1;
		unsigned adp_chrg:1;
		unsigned drvvbus:1;
		unsigned idpullup:1;
		unsigned loopbackenb:1;
		unsigned otgdisable:1;
		unsigned commononn:1;
		unsigned fsel:3;
		unsigned refclksel:2;
		unsigned por:1;
		unsigned vatestenb:2;
		unsigned set_iddq:1;
		unsigned ate_reset:1;
		unsigned fsv_minus:1;
		unsigned fsv_plus:1;
		unsigned bypass_dm_data:1;
		unsigned bypass_dp_data:1;
		unsigned not_used:1;
	} b;
} u2p_r0_t;

typedef struct usb_aml_regs {
	volatile uint32_t usb_r0;
	volatile uint32_t usb_r1;
	volatile uint32_t usb_r2;
	volatile uint32_t usb_r3;
	volatile uint32_t usb_r4;
	volatile uint32_t usb_r5;
	volatile uint32_t usb_r6;
} usb_aml_regs_t;

typedef union usb_r0 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned p30_fsel:6;
		unsigned p30_phy_reset:1;
		unsigned p30_test_powerdown_hsp:1;
		unsigned p30_test_powerdown_ssp:1;
		unsigned p30_acjt_level:5;
		unsigned p30_tx_vboost_lvl:3;
		unsigned p30_lane0_tx2rx_loopbk:1;
		unsigned p30_lane0_ext_pclk_req:1;
		unsigned p30_pcs_rx_los_mask_val:10;
		unsigned u2d_ss_scaledown_mode:2;
		unsigned u2d_act:1;
	} b;
} usb_r0_t;

typedef union usb_r4 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned p21_PORTRESET0:1;
		unsigned p21_SLEEPM0:1;
		unsigned mem_pd:2;
		unsigned reserved4:28;
	} b;
} usb_r4_t;


zx_status_t f_set_usb_phy_config() {
	const int time_dly = 500;

    zx::unowned_resource resource(get_root_resource());

    std::optional<ddk::MmioBuffer> usb_phy;
    auto status = ddk::MmioBuffer::Create(S912_USB_PHY_BASE, S912_USB_PHY_LENGTH, *resource,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE,  &usb_phy);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbInit io_buffer_init_physical failed %d\n", status);
        return status;
    }
    std::optional<ddk::MmioBuffer> preset;
    status = ddk::MmioBuffer::Create(S912_PRESET_BASE, S912_PRESET_LENGTH, *resource,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE,  &preset);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbInit io_buffer_init_physical failed %d\n", status);
        return status;
    }
    std::optional<ddk::MmioBuffer> aobus;
    status = ddk::MmioBuffer::Create(S912_AOBUS_BASE, S912_AOBUS_LENGTH, *resource,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE,  &aobus);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbInit io_buffer_init_physical failed %d\n", status);
        return status;
    }

	volatile u2p_aml_regs_t * u2p_aml_regs = (u2p_aml_regs_t * )((uint8_t*)usb_phy->get() + (PREI_USB_PHY_2_REG_BASE - S912_USB_PHY_BASE));
	volatile usb_aml_regs_t * usb_aml_regs = (usb_aml_regs_t * )((uint8_t*)usb_phy->get() + (PREI_USB_PHY_3_REG_BASE - S912_USB_PHY_BASE));

	u2p_r0_t u2p_r0;
	usb_r0_t usb_r0;
	usb_r4_t usb_r4;

    volatile uint32_t* P_RESET1_REGISTER       = (uint32_t *)((uint8_t*)preset->get() + 0x408);
    volatile uint32_t* P_AO_RTC_ALT_CLK_CNTL0  = (uint32_t *)((uint8_t*)aobus->get() + (0x25 << 2));
    volatile uint32_t* P_AO_RTI_PWR_CNTL_REG0  = (uint32_t *)((uint8_t*)aobus->get() + (0x04 << 2));

printf("f_set_usb_phy_config 1 P_RESET1_REGISTER 0x%p\n", (void*)P_RESET1_REGISTER);
	*P_RESET1_REGISTER = (1<<2);

	*P_AO_RTC_ALT_CLK_CNTL0 |= (1<<31)|(1<<30);
	*P_AO_RTI_PWR_CNTL_REG0 |= (4<<10);

	u2p_r0.d32 = u2p_aml_regs->u2p_r0;
//#if (defined  CONFIG_AML_MESON_GXTVBB)
//	u2p_r0.b.fsel = 5;
//#elif  (defined CONFIG_AML_MESON_GXL)
	u2p_r0.b.fsel = 2;
//#endif
	u2p_r0.b.por = 1;
	u2p_r0.b.dppulldown = 0;
	u2p_r0.b.dmpulldown = 0;
	u2p_aml_regs->u2p_r0 = u2p_r0.d32;

	u2p_r0.d32 = u2p_aml_regs->u2p_r0;
	u2p_r0.b.por = 0;
	u2p_aml_regs->u2p_r0 = u2p_r0.d32;

	usb_r0.d32 = usb_aml_regs->usb_r0;
	usb_r0.b.u2d_act = 1;
	usb_aml_regs->usb_r0 = usb_r0.d32;

	usb_r4.d32 = usb_aml_regs->usb_r4;
	usb_r4.b.p21_SLEEPM0 = 1;
	usb_aml_regs->usb_r4 = usb_r4.d32;

	usleep(time_dly);
	return ZX_OK;
}

#if 0
#define BIT_MASK(start, count) (((1 << (count)) - 1) << (start))
#define SET_BITS(dest, start, count, value) \
    ((dest & ~BIT_MASK(start, count)) | (((value) << (start)) & BIT_MASK(start, count)))
#endif

/*
static const pbus_mmio_t xhci_mmios[] = {
    {
        .base = S912_USB0_BASE,
        .length = S912_USB0_LENGTH,
    },
};

static const pbus_irq_t xhci_irqs[] = {
    {
        .irq = S912_USBH_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const pbus_bti_t xhci_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_USB,
    },
};
*/

static const pbus_mmio_t dwc2_mmios[] = {
    {
        .base = S912_USB1_BASE,
        .length = S912_USB1_LENGTH,
    },
};

static const pbus_irq_t dwc2_irqs[] = {
    {
        .irq = S912_USBD_IRQ,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
};

static const pbus_bti_t dwc2_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_USB,
    },
};


static void print_regs(volatile uint32_t* regs) {
    uint32_t addr = 0xd0078000;
    
    while (addr < 0xd00780a0) {
        printf("%08x: %08x\n", addr, *regs);
        addr += 4;
        regs++;
    }
}

#if 0
static void vim_usb_phy_init(volatile void* regs) {
    volatile void* addr;
    uint32_t temp;

printf("BEFORE\n");
print_regs(regs);

    // amlogic_new_usb2_init
    for (int i = 0; i < 4; i++) {
        addr = regs + (i * PHY_REGISTER_SIZE) + U2P_R0_OFFSET;
        temp = readl(addr);
        temp |= U2P_R0_POR;
        temp |= U2P_R0_DMPULLDOWN;
        temp |= U2P_R0_DPPULLDOWN;
        if (i == 1) {
            temp |= U2P_R0_IDPULLUP;
        }
        writel(temp, addr);
        zx_nanosleep(zx_deadline_after(ZX_USEC(500)));
        temp = readl(addr);
        temp &= ~U2P_R0_POR;
        writel(temp, addr);
    }

/*
    // amlogic_new_usb3_init
    addr = regs + (4 * PHY_REGISTER_SIZE);

    temp = readl(addr + USB_R1_OFFSET);
    temp = SET_BITS(temp, USB_R1_U3H_FLADJ_30MHZ_REG_START, USB_R1_U3H_FLADJ_30MHZ_REG_BITS, 0x20);
    writel(temp, addr + USB_R1_OFFSET);

    temp = readl(addr + USB_R5_OFFSET);
    temp |= USB_R5_IDDIG_EN0;
    temp |= USB_R5_IDDIG_EN1;
    temp = SET_BITS(temp, USB_R5_IDDIG_TH_START, USB_R5_IDDIG_TH_BITS, 255);
    writel(temp, addr + USB_R5_OFFSET);
*/

    if (1 /*device_mode*/) {
        addr = regs + (1 * PHY_REGISTER_SIZE) + U2P_R0_OFFSET;

        temp = readl(addr);
        temp &= ~U2P_R0_DMPULLDOWN;
        temp &= ~U2P_R0_DPPULLDOWN;
        writel(temp, addr);

        addr = regs + (4 * PHY_REGISTER_SIZE) + USB_R0_OFFSET;
        temp = readl(addr);
        temp |= USB_R0_U2D_ACT;
        writel(temp, addr);
  
        addr = regs + (4 * PHY_REGISTER_SIZE) + USB_R4_OFFSET;
        temp = readl(addr);
        temp |= USB_R4_P21_SLEEPM0;
        writel(temp, addr);
  
        addr = regs + (4 * PHY_REGISTER_SIZE) + USB_R1_OFFSET;
        temp = readl(addr);
        temp &= ~(0xf << USB_R1_U3H_HOST_U2_PORT_DISABLE_START);
        temp |= (2 << USB_R1_U3H_HOST_U2_PORT_DISABLE_START);
        writel(temp, addr);
    }


printf("AFTER\n");
print_regs(regs);
}

typedef struct u2p_aml_regs {
	volatile uint32_t u2p_r0;
	volatile uint32_t u2p_r1;
	volatile uint32_t u2p_r2;
} u2p_aml_regs_t;

typedef union u2p_r0 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned bypass_sel:1;
		unsigned bypass_dm_en:1;
		unsigned bypass_dp_en:1;
		unsigned txbitstuffenh:1;
		unsigned txbitstuffen:1;
		unsigned dmpulldown:1;
		unsigned dppulldown:1;
		unsigned vbusvldextsel:1;
		unsigned vbusvldext:1;
		unsigned adp_prb_en:1;
		unsigned adp_dischrg:1;
		unsigned adp_chrg:1;
		unsigned drvvbus:1;
		unsigned idpullup:1;
		unsigned loopbackenb:1;
		unsigned otgdisable:1;
		unsigned commononn:1;
		unsigned fsel:3;
		unsigned refclksel:2;
		unsigned por:1;
		unsigned vatestenb:2;
		unsigned set_iddq:1;
		unsigned ate_reset:1;
		unsigned fsv_minus:1;
		unsigned fsv_plus:1;
		unsigned bypass_dm_data:1;
		unsigned bypass_dp_data:1;
		unsigned not_used:1;
	} b;
} u2p_r0_t;

typedef struct usb_aml_regs {
	volatile uint32_t usb_r0;
	volatile uint32_t usb_r1;
	volatile uint32_t usb_r2;
	volatile uint32_t usb_r3;
	volatile uint32_t usb_r4;
	volatile uint32_t usb_r5;
	volatile uint32_t usb_r6;
} usb_aml_regs_t;

typedef union usb_r0 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned p30_fsel:6;
		unsigned p30_phy_reset:1;
		unsigned p30_test_powerdown_hsp:1;
		unsigned p30_test_powerdown_ssp:1;
		unsigned p30_acjt_level:5;
		unsigned p30_tx_vboost_lvl:3;
		unsigned p30_lane0_tx2rx_loopbk:1;
		unsigned p30_lane0_ext_pclk_req:1;
		unsigned p30_pcs_rx_los_mask_val:10;
		unsigned u2d_ss_scaledown_mode:2;
		unsigned u2d_act:1;
	} b;
} usb_r0_t;

typedef union usb_r4 {
	/** raw register data */
	uint32_t d32;
	/** register bits */
	struct {
		unsigned p21_PORTRESET0:1;
		unsigned p21_SLEEPM0:1;
		unsigned mem_pd:2;
		unsigned reserved4:28;
	} b;
} usb_r4_t;

static void vim_usb_phy_init(volatile uint8_t* regs) {
	u2p_aml_regs_t * u2p_aml_regs = (u2p_aml_regs_t * )(regs + 0x20);
	usb_aml_regs_t * usb_aml_regs = (usb_aml_regs_t * )(regs + 0x80);

	u2p_r0_t u2p_r0;
	usb_r0_t usb_r0;
	usb_r4_t usb_r4;


{
/*
	*P_RESET1_REGISTER = (1<<2);

	*P_AO_RTC_ALT_CLK_CNTL0 |= (1<<31)|(1<<30);
	*P_AO_RTI_PWR_CNTL_REG0 |= (4<<10);
*/

    mmio_buffer_t preset_buf;
    mmio_buffer_init_physical(&preset_buf, S912_PRESET_BASE, S912_PRESET_LENGTH,
                                     get_root_resource(),
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE);
    mmio_buffer_t aobus_buf;
    mmio_buffer_init_physical(&aobus_buf, S912_AOBUS_BASE, S912_AOBUS_LENGTH,
                                     get_root_resource(),
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE);
    mmio_buffer_t hhi_buf;
    mmio_buffer_init_physical(&hhi_buf, HHI_REG_BASE, PAGE_SIZE,
                                     get_root_resource(),
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE);
    
    volatile uint8_t* preset = (uint8_t *)preset_buf.vaddr;
    volatile uint8_t* aobus = (uint8_t *)aobus_buf.vaddr;
    volatile uint8_t* hhi = (uint8_t *)hhi_buf.vaddr;

volatile uint32_t* ptr;

printf("write P_RESET1_REGISTER\n");
    *((volatile unsigned long *)(preset + 0x408)) = (1 << 2);    
printf("write P_AO_RTC_ALT_CLK_CNTL0\n");
    ptr = (volatile uint32_t *)(aobus + (0x25 << 2));
    *ptr |= ((1 << 31) | (1 << 30));
printf("write P_AO_RTI_PWR_CNTL_REG0\n");
    ptr = (volatile uint32_t *)(aobus + (0x04 << 2));
    *ptr |= (4 << 10);
 

    volatile uint32_t* mpeg1 = (volatile uint32_t*)(hhi + (0x51 << 2));
    volatile uint32_t* mpeg2 = (volatile uint32_t*)(hhi + (0x52 << 2));
    volatile uint32_t* usb = (volatile uint32_t*)(hhi + (0x88 << 2));

printf("mpeg1: %08x, mpeg2: %08x usb: %08x\n", *mpeg1, *mpeg2, *usb);



    mmio_buffer_release(&preset_buf);
    mmio_buffer_release(&aobus_buf);
    mmio_buffer_release(&hhi_buf);
}



	u2p_r0.d32 = u2p_aml_regs->u2p_r0;
	u2p_r0.b.fsel = 2;
	u2p_r0.b.por = 1;
	u2p_r0.b.dppulldown = 0;
	u2p_r0.b.dmpulldown = 0;
	u2p_aml_regs->u2p_r0 = u2p_r0.d32;

	u2p_r0.d32 = u2p_aml_regs->u2p_r0;
	u2p_r0.b.por = 0;
	u2p_aml_regs->u2p_r0 = u2p_r0.d32;

	usb_r0.d32 = usb_aml_regs->usb_r0;
	usb_r0.b.u2d_act = 1;
	usb_aml_regs->usb_r0 = usb_r0.d32;

	usb_r4.d32 = usb_aml_regs->usb_r4;
	usb_r4.b.p21_SLEEPM0 = 1;
	usb_aml_regs->usb_r4 = usb_r4.d32;

	usleep(500);
}
#endif

constexpr char kManufacturer[] = "Zircon";
constexpr char kProduct[] = "CDC-Ethernet";
constexpr char kSerial[] = "0123456789ABCDEF";

using FunctionDescriptor = fuchsia_hardware_usb_peripheral_FunctionDescriptor;

static pbus_metadata_t usb_metadata[] = {
    {.type = DEVICE_METADATA_USB_CONFIG, .data_buffer = nullptr, .data_size = 0},
};


zx_status_t Vim::UsbInit() {
    zx_status_t status;
/*
    pbus_dev_t xhci_dev = {};
    xhci_dev.name = "xhci";
    xhci_dev.vid = PDEV_VID_GENERIC;
    xhci_dev.pid = PDEV_PID_GENERIC;
    xhci_dev.did = PDEV_DID_USB_XHCI;
    xhci_dev.mmio_list = xhci_mmios;
    xhci_dev.mmio_count = countof(xhci_mmios);
    xhci_dev.irq_list = xhci_irqs;
    xhci_dev.irq_count = countof(xhci_irqs);
    xhci_dev.bti_list = xhci_btis;
    xhci_dev.bti_count = countof(xhci_btis);
*/

#if 1
    constexpr size_t alignment = alignof(UsbConfig) > __STDCPP_DEFAULT_NEW_ALIGNMENT__
                                     ? alignof(UsbConfig)
                                     : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    UsbConfig* config = reinterpret_cast<UsbConfig*>(
        aligned_alloc(alignment, ROUNDUP(sizeof(UsbConfig) + sizeof(FunctionDescriptor), alignment)));
    if (!config) {
        return ZX_ERR_NO_MEMORY;
    }
    config->vid = GOOGLE_USB_VID;
    config->pid = GOOGLE_USB_FUNCTION_TEST_PID;
    strcpy(config->manufacturer, kManufacturer);
    strcpy(config->serial, kSerial);
    strcpy(config->product, kProduct);
    config->functions[0].interface_class = USB_CLASS_VENDOR;
    config->functions[0].interface_subclass = 0;
    config->functions[0].interface_protocol = 0;
    usb_metadata[0].data_size = sizeof(UsbConfig) + sizeof(FunctionDescriptor);
    usb_metadata[0].data_buffer = config;
#else
    constexpr size_t alignment = alignof(UsbConfig) > __STDCPP_DEFAULT_NEW_ALIGNMENT__
                                     ? alignof(UsbConfig)
                                     : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    constexpr size_t config_size = sizeof(UsbConfig) + 2 * sizeof(FunctionDescriptor);
    UsbConfig* config = reinterpret_cast<UsbConfig*>(
        aligned_alloc(alignment, ROUNDUP(config_size, alignment)));
    if (!config) {
        return ZX_ERR_NO_MEMORY;
    }
    config->vid = GOOGLE_USB_VID;
    config->pid = GOOGLE_USB_CDC_AND_FUNCTION_TEST_PID;
    strcpy(config->manufacturer, kManufacturer);
    strcpy(config->serial, kSerial);
    strcpy(config->product, kProduct);
    config->functions[0].interface_class = USB_CLASS_COMM;
    config->functions[0].interface_subclass = USB_CDC_SUBCLASS_ETHERNET;
    config->functions[0].interface_protocol = 0;
    config->functions[1].interface_class = USB_CLASS_VENDOR;
    config->functions[1].interface_subclass = 0;
    config->functions[1].interface_protocol = 0;
    usb_metadata[0].data_size = config_size;
    usb_metadata[0].data_buffer = config;
#endif
// TODO: delete usb_config later
//    usb_config_ = config;


    pbus_dev_t dwc2_dev = {};
    dwc2_dev.name = "dwc2";
    dwc2_dev.vid = PDEV_VID_GENERIC;
    dwc2_dev.pid = PDEV_PID_GENERIC;
    dwc2_dev.did = PDEV_DID_USB_DWC2;
    dwc2_dev.mmio_list = dwc2_mmios;
    dwc2_dev.mmio_count = countof(dwc2_mmios);
    dwc2_dev.irq_list = dwc2_irqs;
    dwc2_dev.irq_count = countof(dwc2_irqs);
    dwc2_dev.bti_list = dwc2_btis;
    dwc2_dev.bti_count = countof(dwc2_btis);
    dwc2_dev.metadata_list = usb_metadata;
    dwc2_dev.metadata_count = countof(usb_metadata);

#if 0
    std::optional<ddk::MmioBuffer> usb_phy;
    // Please do not use get_root_resource() in new code. See ZX-1467.
    zx::unowned_resource resource(get_root_resource());
    status = ddk::MmioBuffer::Create(S912_USB_PHY_BASE, S912_USB_PHY_LENGTH, *resource,
                                                 ZX_CACHE_POLICY_UNCACHED_DEVICE,  &usb_phy);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbInit io_buffer_init_physical failed %d\n", status);
        return status;
    }

    volatile uint8_t* regs = static_cast<uint8_t*>(usb_phy->get());

    vim_usb_phy_init(regs);


/*
    // amlogic_new_usb2_init
    for (int i = 0; i < 4; i++) {
        volatile uint8_t* addr = regs + (i * PHY_REGISTER_SIZE) + U2P_R0_OFFSET;

        uint32_t temp = readl(addr);
        temp |= U2P_R0_POR;
        temp |= U2P_R0_DMPULLDOWN;
        temp |= U2P_R0_DPPULLDOWN;
        if (i == 1) {
            temp |= U2P_R0_IDPULLUP;
        }
        writel(temp, addr);
        zx_nanosleep(zx_deadline_after(ZX_USEC(500)));
        temp = readl(addr);
        temp &= ~U2P_R0_POR;
        writel(temp, addr);
    }

    // amlogic_new_usb3_init
    volatile uint8_t* addr = regs + (4 * PHY_REGISTER_SIZE);

    uint32_t temp = readl(addr + USB_R1_OFFSET);
    temp = SET_BITS(temp, USB_R1_U3H_FLADJ_30MHZ_REG_START, USB_R1_U3H_FLADJ_30MHZ_REG_BITS, 0x20);
    writel(temp, addr + USB_R1_OFFSET);

    temp = readl(addr + USB_R5_OFFSET);
    temp |= USB_R5_IDDIG_EN0;
    temp |= USB_R5_IDDIG_EN1;
    temp = SET_BITS(temp, USB_R5_IDDIG_TH_START, USB_R5_IDDIG_TH_BITS, 255);
    writel(temp, addr + USB_R5_OFFSET);
*/



/*
    if ((status = pbus_.DeviceAdd(&xhci_dev)) != ZX_OK) {
        zxlogf(ERROR, "UsbInit could not add xhci_dev: %d\n", status);
        return status;
    }
*/
#endif

    if ((status = pbus_.DeviceAdd(&dwc2_dev)) != ZX_OK) {
        zxlogf(ERROR, "UsbInit could not add dwc2_dev: %d\n", status);
        return status;
    }

    return f_set_usb_phy_config();
}
} //namespace vim
