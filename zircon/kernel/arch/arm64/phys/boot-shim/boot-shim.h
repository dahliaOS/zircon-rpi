// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <ktl/type_traits.h>

[[noreturn]] void BootZbi(void*);

// This represents a compile-time fixed item.
// Class must define kPayload_ has a static constexpr object (or array).
// e.g.:
//   struct PlatformId : public ConstantItem<PlatformId, ZBI_TYPE_PLATFORM_ID> {
//     static constexpr zbi_platform_id_t kPayload_{...};
//   };
template <auto Payload, uint32_t Type, uint32_t Extra = 0>
struct ConstantItem {
  static_assert(ktl::is_pointer_v<decltype(Payload)>);
  static constexpr uint32_t type() { return Type; }
  static constexpr uint32_t extra() { return Extra; }
  static constexpr size_t size() { return sizeof(Payload); }
  void FillItem(void* payload) { memcpy(payload, Payload, size()); }
};

// This represents a compile-time fixed ZBI_TYPE_KERNEL_DRIVER item.
// It's used just like ConstantItem, but the parameter is a KDRV_*
// constant rather than a ZBI_TYPE_* constant.
template <typename Class, uint32_t Extra>
using KernelDriver = ConstantItem<Class, ZBI_TYPE_KERNEL_DRIVER, Extra>;

// Shorthand for the common fixed KDRV_ARM_GENERIC_TIMER item.
template <uint32_t Virt, uint32_t Phys = 0>
struct Timer : public KernelDriver<KDRV_ARM_GENERIC_TIMER> {
  static constexpr dcfg_arm_generic_timer_driver_t kPayload_{
      .irq_phys = Phys,
      .irq_virt = Virt,
  };
};

// Shorthand for the common fixed KDRV_ASM_PSCI item.
template <bool UseHvc = false>
struct Psci : public KernelDriver<KDRV_ARM_PSCI> {
  static constexpr dcfg_arm_psci_driver_t kPayload_{
      .use_hvc = UseHvc,
  };
};
