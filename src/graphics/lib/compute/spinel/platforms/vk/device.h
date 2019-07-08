// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_DEVICE_H_
#define SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_DEVICE_H_

//
//
//

#include <vulkan/vulkan_core.h>

#include "allocator_device.h"
#include "allocator_host.h"
#include "spinel_vk.h"

//
//
//

struct spn_device
{
  struct spn_vk_environment * environment;  // Vulkan environment
  struct spn_context *        context;      // Spinel abstract interface
  struct spn_vk *             instance;     // Instance of target state and resources
  struct hotsort_vk *         hs;           // HotSort instance

  struct
  {
    struct
    {
      struct spn_allocator_host_perm perm;
      struct spn_allocator_host_temp temp;  // FIXME(allanmac): we may be able to remove this
    } host;
    struct
    {
      struct
      {
        struct spn_allocator_device_perm local;
        struct spn_allocator_device_perm copyback;  // hrN     -- copy-back to host
        struct spn_allocator_device_perm coherent;  // hw1:drN -- target-specific
      } perm;
      struct
      {
        struct spn_allocator_device_temp local;
      } temp;
    } device;
  } allocator;

  struct spn_queue_pool *  queue_pool;
  struct spn_handle_pool * handle_pool;
  struct spn_dispatch *    dispatch;
  struct spn_block_pool *  block_pool;

#if 0
  struct
  {
    spn_timeline_event_t *        handle_events;
    struct spn_timeline_paths *   paths;
    struct spn_timeline_rasters * rasters;
  } timeline;
#endif

  //
  //
  //
#if 0
  struct spn_scheduler * scheduler;
  struct spn_grid_deps * deps;
#endif
};

//
// FIXME -- Spinel target needs to be able to vend what extensions it
// requires from a target device
//

//
// Creation and disposal intitializes the context and may rely on
// other context resources like the scheduler
//

//
// Disable device because of a fatal error
//

void
spn_device_lost(struct spn_device * const device);

//
//
//

uint64_t
spn_device_wait_nsecs(struct spn_device * const device);

//
// yield : if there are unsignaled fences, test if at least one fence is signaled
// wait  : if there are unsignaled fences, wait for at least one fence to signal
// drain : wait for all unsignaled fences -- unknown if we need this
//

spn_result_t
spn_device_wait(struct spn_device * const device);

spn_result_t
spn_device_yield(struct spn_device * const device);

spn_result_t
spn_device_drain(struct spn_device * const device);

//
//
//

#ifndef SPN_DEVICE_WAIT_DEBUG_DISABLED

spn_result_t
spn_device_wait_verbose(struct spn_device * const device,
                        char const * const        file_line,
                        char const * const        func_name);

#define SPN_DEVICE_WAIT(device_)                                                                   \
  spn_device_wait_verbose(device_, __FILE__ ":" STRINGIFY_MACRO(__LINE__) ":", __func__)

#else

#define SPN_DEVICE_WAIT(device_) spn_device_wait(device_)

#endif

//
//
//

//
//
//

#endif  // SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_DEVICE_H_
