// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "boot-shim.h"
#include "zircon/kernel/phys/main.h"

namespace {

constexpr zbi_platform_id_t kPlatformId = {
    .vid = PDEV_VID_SYNAPTICS,
    .pid = PDEV_PID_SYNAPTICS_AS370,
    .board_name = "as370",
};

template <uint8_t Count>
struct ConstantCpuTopology
    : public ConstantItem<ConstantCpuTopology, ZBI_TYPE_CPU_TOPOLOGY, sizeof(zbi_topology_node_t)> {
  static constexpr auto kCount_ = Count;

  size_t size() const { return kCount_ * sizeof(zbi_topology_node_t); }

  void FillItem(void* payload) const {
    auto node = static_cast<zbi_topology_node_t*>(payload);
    for (decltype(kCount_) index = 0; index < kCount_; ++index) {
      new (node++) zbi_topology_node_t{
          .entity_type = ZBI_TOPOLOGY_ENTITY_PROCESSOR,
          .parent_index = ZBI_TOPOLOGY_NO_PARENT,
          .entity = {
              .processor =
                  {
                      .logical_ids = {index},
                      .logical_id_count = 1,
                      .flags = index == 0 ? ZBI_TOPOLOGY_PROCESSOR_PRIMARY : 0,
                      .architecture = ZBI_TOPOLOGY_ARCH_ARM,
                      .architecture_info = {.arm = {.cpu_id = index, .gic_id = index}},
                  },
          }};
    }
  }
};

struct Gic : public KernelDriver<KDRV_ARM_GIC_V2> {
  static constexpr dcfg_arm_gicv2_driver_t kPayload_{
      .mmio_phys = 0xf7900000,
      .gicd_offset = 0x1000,
      .gicc_offset = 0x2000,
      .ipi_base = 9,
  };
};

struct Memory : public ConstantItem<Memory, ZBI_TYPE_MEM_CONFIG> {
  static constexpr zbi_mem_range_t kPayload_[] = {
      {
          .paddr = 0x02000000,
          .length = 0x20000000,  // 512M
          .type = ZBI_MEM_RANGE_RAM,
      },
      {
          .paddr = 0xf0000000,
          .length = 0x10000000,
          .type = ZBI_MEM_RANGE_PERIPHERAL,
      },
  };
};

struct As370Shim
    : public EmbeddedShim<As370Shim, ConstantCpuTopology<4>, Gic, Memory,
                          ConstantItem<&kPlatformId, ZBI_TYPE_PLATFORM_ID>, Psci<>, Timer<27, 30>> {
  UartDriver<KDRV_DW8250_UART> uart_{0xf7e80c00, 88};
};

}  // namespace

void PhysMain(void* ptr, ArchEarlyTicks ticks) { As370Shim().Main(ptr, ticks); }
