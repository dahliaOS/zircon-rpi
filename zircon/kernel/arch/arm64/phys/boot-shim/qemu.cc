// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "boot-shim.h"
#include "zircon/kernel/phys/main.h"
#include "zircon/kernel/phys/symbolize.h"

namespace {

constexpr zbi_platform_id_t kPlatformId = {
    .vid = PDEV_VID_QEMU,
    .pid = PDEV_PID_QEMU,
    .board_name = "qemu",
};

struct CpuTopology : public DeviceTreeShim::NodeVisitor<CpuTopology> {
  uint32_t count_ = 0;

  void VisitNode(ktl::string_view name) {
    if (name.starts_with("cpu@")) {
      ++count_;
    }
  }

  static constexpr uint32_t type() { return ZBI_TYPE_CPU_TOPOLOGY; }
  static constexpr uint32_t extra() { return sizeof(zbi_topology_node_t); }

  size_t size() const { return count_ * sizeof(zbi_topology_node_t); }

  void FillItem(void* payload) const {
    auto node = static_cast<zbi_topology_node_t*>(payload);
    for (uint32_t index = 0; index < count_; ++index) {
      new (node++) zbi_topology_node_t{
          .entity_type = ZBI_TOPOLOGY_ENTITY_PROCESSOR,
          .parent_index = ZBI_TOPOLOGY_NO_PARENT,
          .entity =
              {
                  .processor =
                      {
                          .logical_ids = {index},
                          .logical_id_count = 1,
                          .flags = index == 0 ? ZBI_TOPOLOGY_PROCESSOR_PRIMARY : 0,
                          .architecture = ZBI_TOPOLOGY_ARCH_ARM,
                          .architecture_info =
                              {
                                  .arm =
                                      {
                                          // qemu seems to put 16 cores per
                                          // aff0 level, max 32 cores.
                                          .cluster_1_id = (index / 16),
                                          .cpu_id = (index % 16),
                                          .gic_id = index,
                                      },
                              },
                      },
              },
      };
    }
  }
};

struct GicVersion : public DeviceTreeShim::PropertyVisitor<GicVersion> {
  uint32_t version_ = 0;

  bool ShouldVisitNode(ktl::string_view name) {
    return name == "intc" || name.starts_with("intc@");
  }

  void VisitProperty(ktl::string_view name, ktl::string_view data) {
    if (name == "compatible") {
      if (data.starts_with("arm,gic-v3")) {
        version_ = KDRV_ARM_GIC_V3;
      } else if (data.starts_with("arm,cortex-a15-gc")) {
        version_ = KDRV_ARM_GIC_V2;
      }
    }
  }

  static constexpr uint32_t type() { return ZBI_TYPE_KERNEL_DRIVER; }
  uint32_t extra() const { return version_; }

  size_t size() const {
    switch (version_) {
      case KDRV_ARM_GIC_V2:
        return sizeof(dcfg_arm_gicv2_driver_t);
      case KDRV_ARM_GIC_V3:
        return sizeof(dcfg_arm_gicv3_driver_t);
      default:
        Panic();
    }
  }

  void FillItem(void* payload) const {
    switch (version_) {
      case KDRV_ARM_GIC_V2:
        new (payload) dcfg_arm_gicv2_driver_t{
            .mmio_phys = 0x08000000,
            .msi_frame_phys = 0x08020000,
            .gicd_offset = 0x00000,
            .gicc_offset = 0x10000,
            .ipi_base = 12,
            .optional = true,
            .use_msi = true,
        };
        break;
      case KDRV_ARM_GIC_V3:
        new (payload) dcfg_arm_gicv3_driver_t{
            .mmio_phys = 0x08000000,
            .gicd_offset = 0x00000,
            .gicr_offset = 0xa0000,
            .gicr_stride = 0x20000,
            .ipi_base = 12,
            .optional = true,
        };
        break;
      default:
        Panic();
    }
  }

  static void Panic() {
    ZX_PANIC("failed to detect gic version from device tree\n");
  }
};

struct QemuShim : public DeviceTreeShim<QemuShim, ConstantItem<&kPlatformId, ZBI_TYPE_PLATFORM_ID>,
                                        Psci<true>, Timer<27, 30>, CpuTopology, GicVersion> {
  uart::qemu::KernelDriver<> uart_;
};

}  // namespace

const char Symbolize::kProgramName_[] = "qemu-boot-shim";

void PhysMain(void* devicetree, ArchEarlyTicks ticks) { QemuShim().Main(devicetree, ticks); }
