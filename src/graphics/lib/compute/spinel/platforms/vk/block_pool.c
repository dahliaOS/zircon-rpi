// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "block_pool.h"

#include "common/vk/vk_assert.h"
#include "device.h"
#include "dispatch.h"
#include "handle_pool.h"
#include "queue_pool.h"
#include "spinel_assert.h"
#include "spn_vk.h"
#include "spn_vk_target.h"

//
//
//

#define SPN_BP_DEBUG

//
//
//

#ifdef SPN_BP_DEBUG

#include <stdio.h>

#include "common/vk/vk_barrier.h"

#define SPN_BP_DEBUG_SIZE ((size_t)1 << 20)

#endif

//
//
//

struct spn_block_pool
{
  struct spn_vk_ds_block_pool_t ds_block_pool;

#ifdef SPN_BP_DEBUG
  struct
  {
    struct
    {
      VkDescriptorBufferInfo * dbi;
      VkDeviceMemory           dm;
    } d;
    struct
    {
      VkDescriptorBufferInfo                  dbi;
      VkDeviceMemory                          dm;
      struct spn_vk_buf_block_pool_bp_debug * mapped;
    } h;
  } bp_debug;
#endif

  struct
  {
    VkDescriptorBufferInfo * dbi;
    VkDeviceMemory           dm;
  } bp_ids;

  struct
  {
    VkDescriptorBufferInfo * dbi;
    VkDeviceMemory           dm;
  } bp_blocks;

  struct
  {
    VkDescriptorBufferInfo * dbi;
    VkDeviceMemory           dm;
  } bp_host_map;

  uint32_t bp_size;
  uint32_t bp_mask;
};

//
//
//

static uint32_t
spn_pow2_ru_u32(uint32_t n)
{
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  n++;

  return n;
}

//
//
//

void
spn_device_block_pool_debug_snap(struct spn_device * const device, VkCommandBuffer cb)
{
  VkBufferCopy const bc = {

    .srcOffset = 0,
    .dstOffset = 0,
    .size      = SPN_VK_BUFFER_OFFSETOF(block_pool, bp_debug, bp_debug) + SPN_BP_DEBUG_SIZE
  };

  vk_barrier_compute_w_to_transfer_r(cb);

  vkCmdCopyBuffer(cb,
                  device->block_pool->bp_debug.d.dbi->buffer,
                  device->block_pool->bp_debug.h.dbi.buffer,
                  1,
                  &bc);
}

void
spn_device_block_pool_debug_print(struct spn_device * const device)
{
  struct spn_vk_buf_block_pool_bp_debug const * const mapped =
    device->block_pool->bp_debug.h.mapped;

  uint32_t const count = mapped->bp_debug_count[0];

  //
  // HEX
  //
  printf("[ %u ] = {", count);

  for (uint32_t ii = 0; ii < count; ii++)
    {
      if ((ii % 32) == 0)
        printf("\n");

      printf("%08X, ", mapped->bp_debug[ii]);
    }

  printf("\n}\n");

  //
  // INT
  //
  printf("[ %u ] = {", count);

  for (uint32_t ii = 0; ii < count; ii++)
    {
      if ((ii % 32) == 0)
        printf("\n");

      printf("%11d, ", mapped->bp_debug[ii]);
    }

  printf("\n}\n");

  //
  // FLOAT
  //
#if 1
  {
    printf("[ %u ] = {", count);

    float const * bp_debug_float = (float *)mapped->bp_debug;

    for (uint32_t ii = 0; ii < count; ii++)
      {
        if ((ii % 32) == 0)
          printf("\n");

        printf("%10.2f, ", bp_debug_float[ii]);
      }

    printf("\n}\n");
  }
#endif

  //
  // COORDS
  //
#if 0
  {
    float const * bp_debug_float = (float *)mapped->bp_debug;

    FILE * file = fopen("debug.segs", "w");

    for (uint32_t ii = 0; ii < count; ii += 4)
      {
        fprintf(file,
                "{ { %10.2f, %10.2f }, { %10.2f, %10.2f } }\n",
                bp_debug_float[ii + 0],
                bp_debug_float[ii + 1],
                bp_debug_float[ii + 2],
                bp_debug_float[ii + 3]);
      }
    fclose(file);
  }
#endif

  //
  // TTS
  //
#if 0
  printf("[ %u ] = {", count);

  for (uint32_t ii = 0; ii < count; ii += 2)
    {
      if ((ii % 2) == 0)
        printf("\n");

      union spn_tts const tts = { .u32 = mapped->bp_debug[ii + 1] };

      printf("%07X : %08X : < %4u | %3d | %4u | %3d > ",
             mapped->bp_debug[ii + 0],
             tts.u32,
             tts.tx,
             tts.dx,
             tts.ty,
             tts.dy);
    }

  printf("\n}\n");
#endif

  //
  // TTRK
  //
#if 0
  printf("[ %u ] = {", count);

  for (uint32_t ii = 0; ii < count; ii += 2)
    {
      if ((ii % 2) == 0)
        printf("\n");

      union spn_ttrk const ttrk = { .u32v2 = { .x = mapped->bp_debug[ii + 0],
                                               .y = mapped->bp_debug[ii + 1] } };

      printf("%08X%08X : < %08X : %4u : %4u : %4u >\n",
             ttrk.u32v2.y,
             ttrk.u32v2.x,
             ttrk.ttsb_id,
             (uint32_t)ttrk.y,
             ttrk.x,
             ttrk.cohort);
    }

  printf("\n}\n");
#endif
}

//
//
//

void
spn_device_block_pool_create(struct spn_device * const device,
                             uint64_t const            block_pool_size,  // in bytes
                             uint32_t const            handle_count)
{
  struct spn_block_pool * const block_pool =
    spn_allocator_host_perm_alloc(&device->allocator.host.perm,
                                  SPN_MEM_FLAGS_READ_WRITE,
                                  sizeof(*block_pool));

  device->block_pool = block_pool;

  struct spn_vk * const                     instance = device->instance;
  struct spn_vk_target_config const * const config   = spn_vk_get_config(instance);

  // block pool sizing
  uint32_t const block_dwords_log2 = config->block_pool.block_dwords_log2;
  uint64_t const block_pool_dwords = (block_pool_size + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  uint32_t const block_dwords      = 1 << block_dwords_log2;

  uint32_t const block_count =
    (uint32_t)((block_pool_dwords + block_dwords - 1) >> block_dwords_log2);

  uint32_t const id_count = spn_pow2_ru_u32(block_count);

  uint32_t const workgroups =
    (block_count + config->block_pool.ids_per_workgroup - 1) / config->block_pool.ids_per_workgroup;

  block_pool->bp_size = block_count;
  block_pool->bp_mask = id_count - 1;

  // get a descriptor set -- there is only one per Spinel device!
  spn_vk_ds_acquire_block_pool(instance, device, &block_pool->ds_block_pool);

  // get descriptor set DBIs
  block_pool->bp_ids.dbi = spn_vk_ds_get_block_pool_bp_ids(instance, block_pool->ds_block_pool);

  block_pool->bp_blocks.dbi =
    spn_vk_ds_get_block_pool_bp_blocks(instance, block_pool->ds_block_pool);

  block_pool->bp_host_map.dbi =
    spn_vk_ds_get_block_pool_bp_host_map(instance, block_pool->ds_block_pool);

#ifdef SPN_BP_DEBUG
  block_pool->bp_debug.d.dbi =
    spn_vk_ds_get_block_pool_bp_debug(instance, block_pool->ds_block_pool);

  size_t const bp_debug_size =
    SPN_VK_BUFFER_OFFSETOF(block_pool, bp_debug, bp_debug) + SPN_BP_DEBUG_SIZE;

  spn_allocator_device_perm_alloc(&device->allocator.device.perm.local,
                                  device->environment,
                                  bp_debug_size,
                                  NULL,
                                  block_pool->bp_debug.d.dbi,
                                  &block_pool->bp_debug.d.dm);

  spn_allocator_device_perm_alloc(&device->allocator.device.perm.copyback,
                                  device->environment,
                                  bp_debug_size,
                                  NULL,
                                  &block_pool->bp_debug.h.dbi,
                                  &block_pool->bp_debug.h.dm);

  vk(MapMemory(device->environment->d,
               block_pool->bp_debug.h.dm,
               0,
               VK_WHOLE_SIZE,
               0,
               (void **)&block_pool->bp_debug.h.mapped));
#endif

  // allocate buffers
  size_t const bp_ids_size =
    SPN_VK_BUFFER_OFFSETOF(block_pool, bp_ids, bp_ids) + id_count * sizeof(spn_block_id_t);

  spn_allocator_device_perm_alloc(&device->allocator.device.perm.local,
                                  device->environment,
                                  bp_ids_size,
                                  NULL,
                                  block_pool->bp_ids.dbi,
                                  &block_pool->bp_ids.dm);

  uint32_t const bp_dwords = block_count * block_dwords;
  size_t const   bp_blocks_size =
    SPN_VK_BUFFER_OFFSETOF(block_pool, bp_blocks, bp_blocks) + bp_dwords * sizeof(uint32_t);

  spn_allocator_device_perm_alloc(&device->allocator.device.perm.local,
                                  device->environment,
                                  bp_blocks_size,
                                  NULL,
                                  block_pool->bp_blocks.dbi,
                                  &block_pool->bp_blocks.dm);

  size_t const bp_host_map_size = SPN_VK_BUFFER_OFFSETOF(block_pool, bp_host_map, bp_host_map) +
                                  handle_count * sizeof(spn_handle_t);

  spn_allocator_device_perm_alloc(&device->allocator.device.perm.local,
                                  device->environment,
                                  bp_host_map_size,
                                  NULL,
                                  block_pool->bp_host_map.dbi,
                                  &block_pool->bp_host_map.dm);

  // update the block pool
  spn_vk_ds_update_block_pool(instance, device->environment, block_pool->ds_block_pool);

  //
  // DISPATCH
  //
  spn_dispatch_id_t id;

  spn(device_dispatch_acquire(device, SPN_DISPATCH_STAGE_BLOCK_POOL, &id));

  VkCommandBuffer cb = spn_device_dispatch_get_cb(device, id);

  // bind the global block pool
  spn_vk_ds_bind_block_pool_init_block_pool(instance, cb, block_pool->ds_block_pool);

  // append push constants
  struct spn_vk_push_block_pool_init const push = { .bp_size = block_pool->bp_size };

  spn_vk_p_push_block_pool_init(instance, cb, &push);

  // bind pipeline
  spn_vk_p_bind_block_pool_init(instance, cb);

  // dispatch the pipeline
  vkCmdDispatch(cb, workgroups, 1, 1);

#ifdef SPN_BP_DEBUG
  vkCmdFillBuffer(cb, block_pool->bp_debug.d.dbi->buffer, 0, sizeof(uint32_t), 0);
#endif

  spn_device_dispatch_submit(device, id);

  //
  // FIXME(allanmac) -- continue intializing and drain the device as
  // late as possible
  //
  spn_device_drain(device);
}

void
spn_device_block_pool_dispose(struct spn_device * const device)
{
  struct spn_vk * const         instance   = device->instance;
  struct spn_block_pool * const block_pool = device->block_pool;

  spn_vk_ds_release_block_pool(instance, block_pool->ds_block_pool);

#ifdef SPN_BP_DEBUG
  spn_allocator_device_perm_free(&device->allocator.device.perm.copyback,
                                 device->environment,
                                 &block_pool->bp_debug.h.dbi,
                                 block_pool->bp_debug.h.dm);

  spn_allocator_device_perm_free(&device->allocator.device.perm.local,
                                 device->environment,
                                 block_pool->bp_debug.d.dbi,
                                 block_pool->bp_debug.d.dm);
#endif

  spn_allocator_device_perm_free(&device->allocator.device.perm.local,
                                 device->environment,
                                 block_pool->bp_host_map.dbi,
                                 block_pool->bp_host_map.dm);

  spn_allocator_device_perm_free(&device->allocator.device.perm.local,
                                 device->environment,
                                 block_pool->bp_blocks.dbi,
                                 block_pool->bp_blocks.dm);

  spn_allocator_device_perm_free(&device->allocator.device.perm.local,
                                 device->environment,
                                 block_pool->bp_ids.dbi,
                                 block_pool->bp_ids.dm);

  spn_allocator_host_perm_free(&device->allocator.host.perm, device->block_pool);
}

//
//
//

uint32_t
spn_device_block_pool_get_mask(struct spn_device * const device)
{
  return device->block_pool->bp_mask;
}

//
//
//

struct spn_vk_ds_block_pool_t
spn_device_block_pool_get_ds(struct spn_device * const device)
{
  return device->block_pool->ds_block_pool;
}

//
//
//
