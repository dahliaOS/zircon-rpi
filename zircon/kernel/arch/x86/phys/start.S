// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/arch/asm.h>
#include <lib/arch/msr.h>
#include <zircon/tls.h>

#include "zircon/kernel/phys/stack.h"

.text

#define CPUID_LEAF(leaf, reg) (cpuid_leaves + (leaf * 4 * 4) + (reg * 4))(%rip)
.macro cpuid_leaf n
.if \n
  mov $\n, %eax
.else
  xor %eax, %eax
.endif
  cpuid
  mov %eax, CPUID_LEAF(\n, 0)
  mov %ebx, CPUID_LEAF(\n, 1)
  mov %ecx, CPUID_LEAF(\n, 2)
  mov %edx, CPUID_LEAF(\n, 3)
.endm

FUNCTION(_start)
  // As early as possible collect the time stamp.
  rdtsc
  shl $32, %rdx
  or %rdx, %rax

  // Clear frame pointer: at the root of the call stack.
  xor %ebp, %ebp

  // Save the incoming argument since %rdi must be clobbered below.
  mov %rdi, %rbx

  // The time stamp will be the second argument to PhysMain.
  mov %rax, %rsi

  // Clear bss.  Note this assumes it's aligned to 8, which is ensured
  // by the declarations in .bss below.
  lea _edata(%rip), %rdi
  lea _end(%rip), %rcx
  sub %rdi, %rcx
  shr $3, %rcx
  xor %eax, %eax
  cld  // Assume nothing.  ABI requires that DF be clear.
  rep stosq

  // Move first argument to PhysMain into place; second is already in %rsi.
  mov %rbx, %rdi

  // Some CPUID bits are immediately useful.  Save the whole leaves for later.
  cpuid_leaf 0
  cmp CPUID_LEAF(0, 0), $1
  jl 0f
  cpuid_leaf 1
  cmp CPUID_LEAF(0, 0), $7
  jl 0f
  cpuid_leaf 7
0:

  // Set up the stacks and the thread pointer area.
  lea boot_thread_pointer(%rip), %rdx

  // Use the MSR if wrgsbase is not available.
  testl CPUID_LEAF(7, 1), $(1 << 0)
  jz .Lno_fsgsbase
  wrgsbase %rdx
  jmp .Lstack_guard
.Lno_fsgsbase
  mov $X86_MSR_IA32_GS_BASE, %ecx
  mov %edx, %eax
  shr $32, %rdx
  wrmsr

.Lstack_guard:
  // Stack guard canary value.  See what kind of randomness is available.
  testl CPUID_LEAF(1, 1), $(1 << 18)
  jz .Lno_rdseed
  rdseed %rax
  jmp .Lstack_guard_write

.Lno_rdseed:
  testl CPUID_LEAF(1, 2), $(1 << 30)
  jz .Lno_rdrand
  rdrand %rax
  jmp .Lstack_guard_write

.Lno_rdrand:
  // The only "randomness" readily available is our own load address, so
  // swizzle that in with some arbitrary bits.
  lea _start(%rip), %rcx
  movabs $0xdeadbeef1ee2d00d, %rax
  xor %rcx, %rax

.Lstack_guard_write:
  mov %rax, %gs:ZX_TLS_STACK_GUARD_OFFSET

  lea (boot_stack + BOOT_STACK_SIZE)(%rip), %rsp
#if __has_feature(safe_stack)
  lea (boot_unsafe_stack + BOOT_STACK_SIZE)(%rip), %rax
  mov %rax, %gs:ZX_TLS_UNSAFE_SP_OFFSET
#endif

  // Now the full C++ ABI is available.  This could theoretically be a tail
  // call since it's obliged never to return, but it's nice to have the
  // caller in a backtrace (and the call implicitly adjusts the stack
  // alignment as the ABI requires).
  call PhysMain

  // Trap forever just in case it does return.
0:ud2
  jmp 0b
FUNCTION_END(_start)

.bss

  .balign 8
LOCAL_DATA(boot_thread_pointer):
#if ZX_TLS_UNSAFE_SP_OFFSET < ZX_TLS_STACK_GUARD_OFFSET
#error "TLS ABI layout??"
#endif
  .skip ZX_TLS_UNSAFE_SP_OFFSET + 8
END(boot_thread_pointer)
