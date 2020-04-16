// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/gpio.h>
#include <ddk/protocol/shareddma.h>
#include <ddktl/metadata/audio.h>
#include <fbl/algorithm.h>
#include <soc/vs680/vs680-clk.h>
#include <soc/vs680/vs680-gpio.h>
#include <soc/vs680/vs680-hw.h>
#include <soc/vs680/vs680-i2c.h>

#include "luis.h"

namespace board_luis {

constexpr zx_bind_inst_t root_match[] = {
    BI_MATCH(),
};
static const zx_bind_inst_t ref_out_i2c_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
    BI_ABORT_IF(NE, BIND_I2C_BUS_ID, 0),
    BI_MATCH_IF(EQ, BIND_I2C_ADDRESS, 0x33),
};
static const zx_bind_inst_t ref_out_codec_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_CODEC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_SYNAPTICS),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_SYNAPTICS_CX9000),
};
static const zx_bind_inst_t dma_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_SHARED_DMA),
};
static const zx_bind_inst_t ref_out_clk0_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_CLOCK),
    BI_MATCH_IF(EQ, BIND_CLOCK_ID, vs680::Clock::kAPll0),
};

static const device_fragment_part_t ref_out_i2c_fragment[] = {
    {fbl::count_of(root_match), root_match},
    {fbl::count_of(ref_out_i2c_match), ref_out_i2c_match},
};
static const device_fragment_part_t ref_out_codec_fragment[] = {
    {fbl::count_of(root_match), root_match},
    {fbl::count_of(ref_out_codec_match), ref_out_codec_match},
};
static const device_fragment_part_t dma_fragment[] = {
    {fbl::count_of(root_match), root_match},
    {fbl::count_of(dma_match), dma_match},
};

static const zx_bind_inst_t ref_out_enable_gpio_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_GPIO),
    BI_MATCH_IF(EQ, BIND_GPIO_PIN, 17),
};
static const device_fragment_part_t ref_out_enable_gpio_fragment[] = {
    {countof(root_match), root_match},
    {countof(ref_out_enable_gpio_match), ref_out_enable_gpio_match},
};
static const device_fragment_part_t ref_out_clk0_fragment[] = {
    {countof(root_match), root_match},
    {countof(ref_out_clk0_match), ref_out_clk0_match},
};

static const device_fragment_t codec_fragments[] = {
    {countof(ref_out_i2c_fragment), ref_out_i2c_fragment},
    {countof(ref_out_enable_gpio_fragment), ref_out_enable_gpio_fragment},
};

static const device_fragment_t controller_fragments[] = {
    {countof(dma_fragment), dma_fragment},
    {countof(ref_out_codec_fragment), ref_out_codec_fragment},
    {countof(ref_out_clk0_fragment), ref_out_clk0_fragment},
};

zx_status_t Luis::AudioInit() {
  constexpr pbus_mmio_t mmios_out[] = {
      {
          .base = vs680::kGlobalBase,
          .length = vs680::kGlobalSize,
      },
      {
          .base = vs680::kAudioGlobalBase,
          .length = vs680::kAudioGlobalSize,
      },
      {
          .base = vs680::kAudioI2sBase,
          .length = vs680::kAudioI2sSize,
      },
  };

  pbus_dev_t controller_out = {};
  controller_out.name = "luis-audio-out";
  controller_out.vid = PDEV_VID_SYNAPTICS;
  controller_out.pid = PDEV_PID_SYNAPTICS_AS370;
  controller_out.did = PDEV_DID_AS370_AUDIO_OUT;
  controller_out.mmio_list = mmios_out;
  controller_out.mmio_count = countof(mmios_out);

  static constexpr pbus_mmio_t mmios_dhub[] = {
      {
          .base = vs680::kAudioDhubBase,
          .length = vs680::kAudioDhubSize,
      },
  };

  constexpr pbus_irq_t irqs_dhub[] = {
      {
          .irq = vs680::kDhubIrq,
          .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
      },
  };
  static constexpr pbus_bti_t btis_dhub[] = {
      {
          .iommu_index = 0,
          .bti_id = BTI_AUDIO_DHUB,
      },
  };

  pbus_dev_t dhub = {};
  dhub.name = "luis-dhub";
  dhub.vid = PDEV_VID_SYNAPTICS;
  dhub.pid = PDEV_PID_SYNAPTICS_AS370;
  dhub.did = PDEV_DID_AS370_DHUB;
  dhub.mmio_list = mmios_dhub;
  dhub.mmio_count = countof(mmios_dhub);
  dhub.irq_list = irqs_dhub;
  dhub.irq_count = countof(irqs_dhub);
  dhub.bti_list = btis_dhub;
  dhub.bti_count = countof(btis_dhub);

  // DMA device.
  auto status = pbus_.DeviceAdd(&dhub);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s adding Dhub failed %d\n", __FILE__, status);
    return status;
  }

  // Output pin assignments.
  gpio_impl_.SetAltFunction(17, 0);  // AMP_EN, 17 mode 0 to set as GPIO.
  gpio_impl_.ConfigOut(17, 1);

  gpio_impl_.SetAltFunction(19, 1);  // I2S1_D0
  gpio_impl_.SetAltFunction(20, 1);  // I2S1_BCLK
  gpio_impl_.SetAltFunction(21, 1);  // I2S1_LKCLK

  // Output devices.
  constexpr zx_device_prop_t props[] = {{BIND_PLATFORM_DEV_VID, 0, PDEV_VID_SYNAPTICS},
                                        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_SYNAPTICS_CX9000}};

  const composite_device_desc_t comp_desc = {
      .props = props,
      .props_count = countof(props),
      .fragments = codec_fragments,
      .fragments_count = countof(codec_fragments),
      .coresident_device_index = UINT32_MAX,
      .metadata_list = nullptr,
      .metadata_count = 0,
  };

  status = DdkAddComposite("audio-codec", &comp_desc);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s DdkAddComposite failed %d\n", __FILE__, status);
    return status;
  }

  // coresident_device_index = 1 to share devhost with DHub.
  // When autoproxying (ZX-3478) or its replacement is in place,
  // we can have these drivers in different devhosts.
  constexpr uint32_t controller_coresident_device_index = 1;
  status =
      pbus_.CompositeDeviceAdd(&controller_out, controller_fragments, countof(controller_fragments),
                               controller_coresident_device_index);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s adding audio controller out device failed %d\n", __FILE__, status);
    return status;
  }

  return ZX_OK;
}

}  // namespace board_luis
