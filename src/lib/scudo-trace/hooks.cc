#include <trace/event.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

#include <assert.h>
#include <stdio.h>

#include "src/lib/fxl/logging.h"
#include "zircon/system/ulib/trace-engine/include/lib/trace-engine/types.h"
#include "zircon/third_party/ulib/scudo/interface.h"

__BEGIN_CDECLS

unsigned int allocations = 0;
unsigned int deallocations = 0;

__EXPORT  void __scudo_allocate_hook(void *ptr, size_t size) {
  TRACE_ASYNC_BEGIN("memory", "alloc", static_cast<trace_async_id_t>(ptr));
  ++allocations;
}

__EXPORT void __scudo_deallocate_hook(void *ptr) {
  TRACE_ASYNC_END("memory", "alloc", static_cast<trace_async_id_t>(ptr));
  ++deallocations;
}

__END_CDECLS

namespace scudo_trace {

void ExportScudoStats(bool silent) {
  if (!silent) {
    __scudo_print_stats();
    FXL_LOG(INFO) << allocations << " allocs, " << deallocations << " deallocs";
  }
  TRACE_COUNTER("system", "alloc_stats", 0,
                "allocs", TA_UINT32(allocations),
                "deallocs", TA_UINT32(deallocations));
}

}  // namespace scudo_trace
