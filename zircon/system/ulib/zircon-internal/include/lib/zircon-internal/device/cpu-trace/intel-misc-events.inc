// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Database of miscellaneous Intel-specific performance monitor registers.
// This has the benefit of providing the data in a usage-independent way.
// See, e.g., desktop-6th-gen-core-family-datasheet-vol-2.

#ifndef DEF_MISC_ARCH_EVENT
#error DEF_MISC_ARCH_EVENT not defined
#endif

// Miscellaneous performance counters
// args:
// - symbol (used in code, must be unique for all symbols within an arch)
//   (must be VALID_C_SYMBOL)
// - event name (used in trace specification files)
//   (must be valid_c_symbol, without the group prefix,
//   i.e., instructions_retired not fixed_instructions_retired)
// - id (10 bits, must be unique for each DEF_MISC_ARCH_EVENT entry)
// - offset (the offset from BAR of the value)
// - size (in bits, either 32 or 64)
// - flags (IPM_MISC_REG_FLAG_* values) (uint32_t) [at present there are none]
// - readable name (ideally max 24 chars for legibility in display in chrome)
// - description (if empty use "")
//
// These counters are kinda like fixed counters except there's no need to
// specify a h/w register number: there isn't any.
// Misc counter id rule: |id| must be <= PERFMON_MAX_EVENT.
//
// All current misc counters require a timebase, so that is the default
// for misc regs. This affects configuration: If one of these is requested
// in sampling mode and there is no timebase, then configuration fails.

DEF_MISC_ARCH_EVENT(MISC_PKG_RAPL_POWER_PKG,
  rapl_pkg_power_mj,
  1, 0x58f0, 32, 0,
  "RAPL Package Power Usage", "Running average power usage for the package domain")
DEF_MISC_ARCH_EVENT(MISC_PKG_RAPL_POWER_PP0,
  rapl_pp0_power_mj,
  2, 0x58f0, 32, 0,
  "RAPL PP0 Power Usage", "Running average power usage for PP0 domain (CPU core)")
DEF_MISC_ARCH_EVENT(MISC_PKG_RAPL_POWER_PP1,
  rapl_pp1_power_mj,
  3, 0x58f0, 32, 0,
  "RAPL PP1 Power Usage", "Running average power usage for PP1 domain (uncore)")
DEF_MISC_ARCH_EVENT(MISC_PKG_RAPL_POWER_DRAM,
  rapl_dram_power_mj,
  4, 0x58f0, 32, 0,
  "RAPL PP1 Power Usage", "Running average power usage for DRAM domain")

#undef DEF_MISC_ARCH_EVENT
