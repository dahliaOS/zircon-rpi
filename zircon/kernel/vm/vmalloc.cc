// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "vm/vmalloc.h"

#include <trace.h>

#include <vm/vm_aspace.h>
#include <vm/vm_object_paged.h>

#define LOCAL_TRACE 0

static const uint kArchRwFlags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;
static fbl::RefPtr<VmAddressRegion> vmalloc_vmar;

// the power of 2 size of the vmar used for vmalloc mappings
static const size_t vmalloc_vmar_shift = 30;  // 1GB

void* vmalloc(size_t len, const char* _name) {
  const char* name = _name ? _name : "vmalloc";

  // Create a VMO for our allocation
  fbl::RefPtr<VmObject> vmo;
  zx_status_t status = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, 0u, len, &vmo);
  if (status != ZX_OK) {
    TRACEF("vmalloc: failed to allocate vmo of size %zu\n", len);
    return nullptr;
  }
  vmo->set_name(name, strlen(name));

  // get the rounded up vmo size for mapping
  auto vmo_size = vmo->size();

  // create a mapping with random placement into the vmalloc region
  fbl::RefPtr<VmMapping> mapping;
  status = vmalloc_vmar->CreateVmMapping(0, vmo_size, 0, 0, std::move(vmo), 0, kArchRwFlags, name,
                                         &mapping);
  if (status != ZX_OK)
    return nullptr;

  // fault in all the pages so we dont demand fault in the allocation
  status = mapping->MapRange(0, vmo_size, true);
  if (status != ZX_OK) {
    mapping->Destroy();
    return nullptr;
  }

  void* ptr = reinterpret_cast<void*>(mapping->base());

  LTRACEF("returning %p for size %zu\n", ptr, len);

  return ptr;
}

void vmfree(void* ptr) {
  LTRACEF("ptr %p\n", ptr);

  vaddr_t va = reinterpret_cast<vaddr_t>(ptr);

  DEBUG_ASSERT(is_kernel_address(va));

  zx_status_t status = VmAspace::kernel_aspace()->FreeRegion(va);
  if (status != ZX_OK) {
    TRACEF("warning: vmfree at %p failed\n", ptr);
  }
}

void vmalloc_init() {
  LTRACE_ENTRY;

  auto root_vmar = VmAspace::kernel_aspace()->RootVmar()->as_vm_address_region();

  zx_status_t status = root_vmar->CreateSubVmar(0, (1ULL << vmalloc_vmar_shift), vmalloc_vmar_shift,
                                                VMAR_FLAG_CAN_MAP_READ | VMAR_FLAG_CAN_MAP_WRITE,
                                                "vmalloc vmar", &vmalloc_vmar);
  ASSERT(status == ZX_OK);
  DEBUG_ASSERT(vmalloc_vmar);
}
