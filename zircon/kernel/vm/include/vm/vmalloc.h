// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_VM_INCLUDE_VM_VMALLOC_H_
#define ZIRCON_KERNEL_VM_INCLUDE_VM_VMALLOC_H_

#include <stddef.h>

// Convenience routines to allocate page aligned chunks of kernel space.
// Each of the allocations are contained within a unique VMO + VM mapping.
// Size will be rounded up to nearest page size.
//
// Note: memory will start off fully mapped and it's always safe to assume
// the memory is zero filled.
void* vmalloc(size_t len, const char* name);

// Free the vmalloc region that the pointer is within.
void vmfree(void* ptr);

// Called once at init.
void vmalloc_init();

#endif  // ZIRCON_KERNEL_VM_INCLUDE_VM_VMALLOC_H_
