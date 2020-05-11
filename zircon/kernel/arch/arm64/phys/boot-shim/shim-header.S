// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/arch/asm.h>

#define SCTLR_I     (1 << 12)   // Instruction cache enable
#define SCTLR_C     (1 << 2)    // Cache enable
#define SCTLR_M     (1 << 0)    // MMU enable

.macro disable_caches_and_mmu sctrl_elx, scratch
  mrs \scratch, \sctrl_elx
  bic \scratch, \scratch, #(SCTLR_I | SCTLR_C | SCTLR_M)
  msr \sctlr_elx, \scratch
.endm

.section .boot.header, "a", %progbits

  .balign KERNEL_ALIGN

.function _uefi_start, global, cfi=custom, nosection=nosection

  // There is no stack yet.  There is no way to say the CFA is undefined,
  // so say it's zero.  The return address is undefined.
  .cfi_cfi_escape DW_CFA_def_cfa_expression, 1, DW_OP_lit0
  .cfi_undefined x30

  // Magic instruction that gives us UEFI "MZ" signature.
  add x13, x18, #0x16
  b 0f				// Jump over the rest of the magic header.

  .quad 0			// Image offset from start of RAM (unused).
  .quad 0			// Image size (unused).
  .quad 0
  .quad 0
  .quad 0
  .quad 0

  // ARM64 magic number.
  .ascii "ARM\x64"
  .balign 8

0:
  // Don't touch the argument register, x0.

  // Disable caches and MMU since we don't know what state we're started in.
  // Before this it might not be safe to access any memory at all.

  mrs x16, CurrentEL		// What EL are we running at?
  cmp x16, #(1 << 2)		// Is it EL1?
  beq 1f
  disable_caches_and_mmu sctlr_el2, x16
  b 2f
1:disable_caches_and_mmu sctlr_el1, x16
2:dsb sy
  isb

  // Jump to generic phys entry code.
  // It will pass along the argument in x0.
  b _start

.end_function
