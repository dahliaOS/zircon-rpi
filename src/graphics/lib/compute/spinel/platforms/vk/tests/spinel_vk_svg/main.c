// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <float.h>
#include <stdio.h>
#include <stdlib.h>

#include "allocator_device.h"
#include "common/macros.h"
#include "common/vk/vk_assert.h"
#include "common/vk/vk_cache.h"
#include "common/vk/vk_debug.h"
#include "ext/color/color.h"
#include "ext/transform_stack/transform_stack.h"
#include "spinel/spinel_assert.h"
#include "spinel/spinel_vk.h"

//
//
//

#include "spinel_vk_find_target.h"

//
//
//

#if defined(SPN_VK_SHADER_INFO_AMD_STATISTICS) || defined(SPN_VK_SHADER_INFO_AMD_DISASSEMBLY)
#include "common/vk/vk_shader_info_amd.h"
#endif

//
// Define a platform-specific prefix
//

#ifdef __Fuchsia__
#define VK_PIPELINE_CACHE_PREFIX_STRING "/cache/."
#else
#define VK_PIPELINE_CACHE_PREFIX_STRING "."
#endif

//
// clang-format off
//

#define SPN_BUFFER_SURFACE_WIDTH  1024
#define SPN_BUFFER_SURFACE_HEIGHT 1024
#define SPN_BUFFER_SURFACE_PIXELS (SPN_BUFFER_SURFACE_WIDTH * SPN_BUFFER_SURFACE_HEIGHT)
#define SPN_BUFFER_SURFACE_SIZE   (SPN_BUFFER_SURFACE_PIXELS * 4 * sizeof(SPN_BUFFER_SURFACE_CHANNEL_TYPE))

//
// clang-format on
//

//
// FIXME(allanmac): Styling opcodes will be buried later
//

//
// clang-format off
//

#define SPN_STYLING_OPCODE_NOOP                        0

#define SPN_STYLING_OPCODE_COVER_NONZERO               1
#define SPN_STYLING_OPCODE_COVER_EVENODD               2
#define SPN_STYLING_OPCODE_COVER_ACCUMULATE            3
#define SPN_STYLING_OPCODE_COVER_MASK                  4

#define SPN_STYLING_OPCODE_COVER_WIP_ZERO              5
#define SPN_STYLING_OPCODE_COVER_ACC_ZERO              6
#define SPN_STYLING_OPCODE_COVER_MASK_ZERO             7
#define SPN_STYLING_OPCODE_COVER_MASK_ONE              8
#define SPN_STYLING_OPCODE_COVER_MASK_INVERT           9

#define SPN_STYLING_OPCODE_COLOR_FILL_SOLID            10
#define SPN_STYLING_OPCODE_COLOR_FILL_GRADIENT_LINEAR  11

#define SPN_STYLING_OPCODE_COLOR_WIP_ZERO              12
#define SPN_STYLING_OPCODE_COLOR_ACC_ZERO              13

#define SPN_STYLING_OPCODE_BLEND_OVER                  14
#define SPN_STYLING_OPCODE_BLEND_PLUS                  15
#define SPN_STYLING_OPCODE_BLEND_MULTIPLY              16
#define SPN_STYLING_OPCODE_BLEND_KNOCKOUT              17

#define SPN_STYLING_OPCODE_COVER_WIP_MOVE_TO_MASK      18
#define SPN_STYLING_OPCODE_COVER_ACC_MOVE_TO_MASK      19

#define SPN_STYLING_OPCODE_COLOR_ACC_OVER_BACKGROUND   20
#define SPN_STYLING_OPCODE_COLOR_ACC_STORE_TO_SURFACE  21
#define SPN_STYLING_OPCODE_COLOR_ACC_TEST_OPACITY      22

#define SPN_STYLING_OPCODE_COLOR_ILL_ZERO              23
#define SPN_STYLING_OPCODE_COLOR_ILL_COPY_ACC          24
#define SPN_STYLING_OPCODE_COLOR_ACC_MULTIPLY_ILL      25

#define SPN_STYLING_OPCODE_COUNT                       26

//
// clang-format on
//

//
// Temporary forward decls
//

spn_path_t *
lion_cub_paths(spn_path_builder_t pb, uint32_t * const path_count);

spn_raster_t *
lion_cub_rasters(spn_raster_builder_t           rb,
                 struct transform_stack * const ts,
                 uint32_t const                 rotations,
                 spn_path_t const * const       paths,
                 uint32_t const                 path_count,
                 uint32_t * const               raster_count);

spn_layer_id *
lion_cub_composition(spn_composition_t          composition,
                     spn_raster_t const * const rasters,
                     uint32_t const             raster_count,
                     uint32_t * const           layer_count);

void
lion_cub_styling(spn_styling_t              styling,
                 spn_group_id const         group_id,
                 spn_layer_id const * const layer_ids,
                 uint32_t const             layer_count);

//
//
//

void
spn_buffer_to_ppm(void * mapped, uint32_t const surface_width, uint32_t const surface_height)
{
#ifndef SPN_BUFFER_SURFACE_CHANNEL_TYPE_IS_FLOAT
#define SPN_BUFFER_SURFACE_CHANNEL_TYPE uint8_t
#else
#define SPN_BUFFER_SURFACE_CHANNEL_TYPE float
#endif

  struct spn_main_rgba
  {
    SPN_BUFFER_SURFACE_CHANNEL_TYPE r;
    SPN_BUFFER_SURFACE_CHANNEL_TYPE g;
    SPN_BUFFER_SURFACE_CHANNEL_TYPE b;
    SPN_BUFFER_SURFACE_CHANNEL_TYPE a;
  };

  FILE *                       file = fopen("surface.ppm", "wb");
  struct spn_main_rgba const * rgba = mapped;

  fprintf(file, "P6\n%u %u\n255\n", surface_width, surface_height);

  for (uint32_t ii = 0; ii < surface_width * surface_height; ii++)
    {
#ifndef SPN_BUFFER_SURFACE_CHANNEL_TYPE_IS_FLOAT
      struct spn_main_rgba const * const rgb = rgba + ii;
#else
      uint8_t const rgb[3] = {

        (uint8_t)(rgba[ii].r * 255),
        (uint8_t)(rgba[ii].g * 255),
        (uint8_t)(rgba[ii].b * 255)

      };
#endif

      fwrite(rgb, 1, 3, file);  // RGB
    }

  fclose(file);
}

//
//
//

int
main(int argc, char const * argv[])
{
  //
  // create a Vulkan instances
  //
  VkApplicationInfo const app_info = {

    .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pNext              = NULL,
    .pApplicationName   = "Fuchsia Spinel/VK Test",
    .applicationVersion = 0,
    .pEngineName        = "Fuchsia Spinel/VK",
    .engineVersion      = 0,
    .apiVersion         = VK_API_VERSION_1_1
  };

  char const * const instance_enabled_layers[] = { "VK_LAYER_LUNARG_standard_validation", NULL };

  char const * const instance_enabled_extensions[] = { VK_EXT_DEBUG_REPORT_EXTENSION_NAME, NULL };

  VkInstanceCreateInfo const instance_info = {

    .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pNext                   = NULL,
    .flags                   = 0,
    .pApplicationInfo        = &app_info,
    .enabledLayerCount       = ARRAY_LENGTH_MACRO(instance_enabled_layers) - 1,
    .ppEnabledLayerNames     = instance_enabled_layers,
    .enabledExtensionCount   = ARRAY_LENGTH_MACRO(instance_enabled_extensions) - 1,
    .ppEnabledExtensionNames = instance_enabled_extensions
  };

  VkInstance instance;

  vk(CreateInstance(&instance_info, NULL, &instance));

  //
  //
  //
#ifndef NDEBUG
  PFN_vkCreateDebugReportCallbackEXT vkCreateDebugReportCallbackEXT =
    (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance,
                                                              "vkCreateDebugReportCallbackEXT");

  PFN_vkDestroyDebugReportCallbackEXT vkDestroyDebugReportCallbackEXT =
    (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(instance,
                                                               "vkDestroyDebugReportCallbackEXT");

  VkDebugReportFlagsEXT const drf = VK_DEBUG_REPORT_INFORMATION_BIT_EXT |
                                    VK_DEBUG_REPORT_WARNING_BIT_EXT |
                                    VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
                                    VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;

  struct VkDebugReportCallbackCreateInfoEXT const drcci = {

    .sType       = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
    .pNext       = NULL,
    .flags       = drf,
    .pfnCallback = vk_debug_report_cb,
    .pUserData   = NULL
  };

  VkDebugReportCallbackEXT drc;

  vk(CreateDebugReportCallbackEXT(instance, &drcci, NULL, &drc));
#endif

  //
  // Prepare Vulkan environment for Spinel
  //
  struct spn_vk_environment environment = { .d   = VK_NULL_HANDLE,
                                            .ac  = NULL,
                                            .pc  = VK_NULL_HANDLE,
                                            .pd  = VK_NULL_HANDLE,
                                            .qfi = 0 };

  //
  // acquire all physical devices
  //
  uint32_t pd_count;

  vk(EnumeratePhysicalDevices(instance, &pd_count, NULL));

  if (pd_count == 0)
    {
      fprintf(stderr, "No device found\n");

      return EXIT_FAILURE;
    }

  VkPhysicalDevice * pds = malloc(pd_count * sizeof(*pds));

  vk(EnumeratePhysicalDevices(instance, &pd_count, pds));

  //
  // select the first device if *both* ids aren't provided
  //
  VkPhysicalDeviceProperties pdp;

  vkGetPhysicalDeviceProperties(pds[0], &pdp);

  uint32_t const vendor_id = (argc <= 2) ? pdp.vendorID : strtoul(argv[1], NULL, 16);
  uint32_t const device_id = (argc <= 2) ? pdp.deviceID : strtoul(argv[2], NULL, 16);

  //
  // list all devices
  //
  environment.pd = VK_NULL_HANDLE;

  for (uint32_t ii = 0; ii < pd_count; ii++)
    {
      VkPhysicalDeviceProperties pdp_tmp;

      vkGetPhysicalDeviceProperties(pds[ii], &pdp_tmp);

      bool const is_match = (pdp_tmp.vendorID == vendor_id) && (pdp_tmp.deviceID == device_id);

      if (is_match)
        {
          pdp            = pdp_tmp;
          environment.pd = pds[ii];
        }

      fprintf(stdout,
              "%c %X : %X : %s\n",
              is_match ? '*' : ' ',
              pdp_tmp.vendorID,
              pdp_tmp.deviceID,
              pdp_tmp.deviceName);
    }

  if (environment.pd == VK_NULL_HANDLE)
    {
      fprintf(stderr, "Device %X : %X not found.\n", vendor_id, device_id);

      return EXIT_FAILURE;
    }

  free(pds);

  //
  // get the physical device's memory props
  //
  vkGetPhysicalDeviceMemoryProperties(environment.pd, &environment.pdmp);

  //
  // get image properties
  //
  // vkGetPhysicalDeviceImageFormatProperties()
  //
  // vk(GetPhysicalDeviceImageFormatProperties(phy_device,
  //

  //
  // get queue properties
  //
  // FIXME(allanmac): The number and composition of queues (compute
  // vs. graphics) will be configured by the target.
  //
  // This implies Spinel/VK needs to either create the queue pool itself
  // or accept an externally defined queue strategy.
  //
  // This is moot until we get Timeline Semaphores and can run on
  // multiple queues.
  //
  uint32_t qfc;

  vkGetPhysicalDeviceQueueFamilyProperties(environment.pd, &qfc, NULL);

  VkQueueFamilyProperties qfp[qfc];

  vkGetPhysicalDeviceQueueFamilyProperties(environment.pd, &qfc, qfp);

  //
  // create queue
  //
  float const qp[] = { 1.0f };

  VkDeviceQueueCreateInfo const qi = {

    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .pNext            = NULL,
    .flags            = 0,
    .queueFamilyIndex = environment.qfi,
    .queueCount       = 1,
    .pQueuePriorities = qp
  };

  //
  // clumsily enable AMD GCN shader info extension
  //
  char const * const device_enabled_extensions[] = {
#if defined(SPN_VK_SHADER_INFO_AMD_STATISTICS) || defined(SPN_VK_SHADER_INFO_AMD_DISASSEMBLY)
    VK_AMD_SHADER_INFO_EXTENSION_NAME
#else
    NULL
#endif
  };

  uint32_t device_enabled_extension_count = 0;

#if defined(SPN_VK_SHADER_INFO_AMD_STATISTICS) || defined(SPN_VK_SHADER_INFO_AMD_DISASSEMBLY)
  if (pdp.vendorID == 0x1002)
    device_enabled_extension_count = 1;
#endif

  //
  //
  //
  VkPhysicalDeviceFeatures device_features = { false };

  //
  // FIXME -- for now, HotSort requires 'shaderInt64'
  //
  if (true /*key_val_words == 2*/)
    {
      //
      // FIXME
      //
      // SEGMENT_TTCK and SEGMENT_TTRK shaders benefit from
      // shaderInt64 but shaderFloat64 shouldn't be required.
      //
      device_features.shaderInt64   = true;
      device_features.shaderFloat64 = true;
    }

  VkDeviceCreateInfo const device_info = {

    .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext                   = NULL,
    .flags                   = 0,
    .queueCreateInfoCount    = 1,
    .pQueueCreateInfos       = &qi,
    .enabledLayerCount       = 0,
    .ppEnabledLayerNames     = NULL,
    .enabledExtensionCount   = device_enabled_extension_count,
    .ppEnabledExtensionNames = device_enabled_extensions,
    .pEnabledFeatures        = &device_features
  };

  vk(CreateDevice(environment.pd, &device_info, NULL, &environment.d));

  //
  // create the pipeline cache
  //
  vk_ok(vk_pipeline_cache_create(environment.d,
                                 NULL,
                                 VK_PIPELINE_CACHE_PREFIX_STRING "vk_cache",
                                 &environment.pc));

  //
  // create device perm allocators
  //
  struct spn_allocator_device_perm perm_device_local;

  {
    VkMemoryPropertyFlags const mpf = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkBufferUsageFlags const buf = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    spn_allocator_device_perm_create(&perm_device_local, &environment, mpf, buf, 0, NULL);
  }

  struct spn_allocator_device_perm perm_host_visible;

  {
    VkMemoryPropertyFlags const mpf = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

    VkBufferUsageFlags const buf = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    spn_allocator_device_perm_create(&perm_host_visible, &environment, mpf, buf, 0, NULL);
  }

  //
  // allocate surfaces
  //
  struct surface
  {
    struct
    {
      VkDescriptorBufferInfo dbi;
      VkDeviceMemory         dm;
    } d;
    struct
    {
      VkDescriptorBufferInfo dbi;
      VkDeviceMemory         dm;
      void *                 map;
    } h;
  } surface;

  VkDeviceSize const surface_size = SPN_BUFFER_SURFACE_SIZE;

  spn_allocator_device_perm_alloc(&perm_device_local,
                                  &environment,
                                  surface_size,
                                  0,
                                  &surface.d.dbi,
                                  &surface.d.dm);

  spn_allocator_device_perm_alloc(&perm_host_visible,
                                  &environment,
                                  surface_size,
                                  0,
                                  &surface.h.dbi,
                                  &surface.h.dm);

  vk(MapMemory(environment.d, surface.h.dm, 0, VK_WHOLE_SIZE, 0, (void **)&surface.h.map));

  //
  // find Spinel target
  //
  struct spn_vk_context_create_info create_info = {
    .block_pool_size = 1 << 26,  // 64 MB
    .handle_count    = 1 << 17,  // 128K handles
  };

  char error_message[256];

  if (!spn_vk_find_target(vendor_id,
                          device_id,
                          &create_info.spn,
                          &create_info.hotsort,
                          error_message,
                          sizeof(error_message)))
    {
      fprintf(stderr, "%s\n", error_message);
      return EXIT_FAILURE;
    }

  //
  // create a Spinel context
  //
  spn_context_t context;

  spn(vk_context_create(&environment, &create_info, &context));

  //
  // create a transform stack
  //
  struct transform_stack * const ts = transform_stack_create(16);

  transform_stack_push_scale(ts, 32.0f, 32.0f);

  ////////////////////////////////////
  //
  // SPINEL BOILERPLATE
  //

  //
  // create builders
  //
  spn_path_builder_t pb;

  spn(path_builder_create(context, &pb));

  spn_raster_builder_t rb;

  spn(raster_builder_create(context, &rb));

  //
  // create composition
  //
  spn_composition_t composition;

  spn(composition_create(context, &composition));

  //
  // min/max layer in top level group
  //
  uint32_t const layer_count = 4096;
  uint32_t const layer_max   = layer_count - 1;
  uint32_t const layer_min   = 0;

  //
  // create styling
  //
  spn_styling_t styling;

  spn(styling_create(context, &styling, layer_count, 16384));  // 4K layers, 16K cmds

  //
  // define top level styling group
  //
  spn_group_id group_id;

  spn(styling_group_alloc(styling, &group_id));

  {
    spn_styling_cmd_t * cmds_enter;

    spn(styling_group_enter(styling, group_id, 1, &cmds_enter));

    cmds_enter[0] = SPN_STYLING_OPCODE_COLOR_ACC_ZERO;
  }

  {
    spn_styling_cmd_t * cmds_leave;

    spn(styling_group_leave(styling, group_id, 4, &cmds_leave));

    float const background[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // cmds[0-2]
    spn_styling_background_over_encoder(cmds_leave, background);

    cmds_leave[3] = SPN_STYLING_OPCODE_COLOR_ACC_STORE_TO_SURFACE;
  }

  // this is the root group
  spn(styling_group_parents(styling, group_id, 0, NULL));

  // the range of the root group is maximal
  spn(styling_group_range_lo(styling, group_id, layer_min));
  spn(styling_group_range_hi(styling, group_id, layer_max));

  //
  // loop over the entire pipeline
  //
  for (uint32_t ii = 0; ii < 1; ii++)
    {
      //
      // define paths
      //
      uint32_t           path_count;
      spn_path_t * const paths = lion_cub_paths(pb, &path_count);

      // force start the pipeline -- FYI: flush() is deprecated and will be removed
      // spn(path_builder_flush(pb));
      // spn_context_drain(context);

      printf("PATH_BUILDER\n");

      //
      // define rasters
      //
      uint32_t             raster_count;
      spn_raster_t * const rasters = lion_cub_rasters(rb, ts, 1, paths, path_count, &raster_count);

      // force start the pipeline -- FYI: flush() is deprecated and will be removed
      // spn(raster_builder_flush(rb));
      // spn_context_drain(context);

      printf("RASTER_BUILDER\n");

      //
      // place rasters into composition
      //
      uint32_t             layer_count;
      spn_layer_id * const layer_ids =
        lion_cub_composition(composition, rasters, raster_count, &layer_count);

      // spn_composition_seal(composition);
      // spn_context_drain(context);

      printf("COMPOSITION\n");

      //
      // add to styling
      //
      lion_cub_styling(styling, group_id, layer_ids, layer_count);

      // spn_styling_seal(styling);
      // spn_context_drain(context);

      printf("STYLING\n");

      //
      // render
      //
      spn_render_submit_ext_vk_buffer_copy_t rs_buffer_copy = {

        .ext      = NULL,
        .type     = SPN_RENDER_SUBMIT_EXT_TYPE_VK_BUFFER_COPY,
        .dst      = surface.h.dbi,
        .dst_size = SPN_BUFFER_SURFACE_SIZE
      };

      spn_render_submit_ext_vk_buffer_t rs_buffer = {

        .ext           = &rs_buffer_copy,
        .type          = SPN_RENDER_SUBMIT_EXT_TYPE_VK_BUFFER,
        .surface       = surface.d.dbi,
        .surface_pitch = SPN_BUFFER_SURFACE_WIDTH,
        .si            = NULL
      };

      spn_render_submit_t const rs = {

        .ext         = &rs_buffer,
        .styling     = styling,
        .composition = composition,
        .tile_clip   = { 0, 0, UINT32_MAX, UINT32_MAX }
      };

      spn(render(context, &rs));

      //
      // drain everything
      //
      spn_context_drain(context);

      printf("RENDER\n");

      //
      // unseal
      //
      spn(composition_unseal(composition));
      spn(composition_reset(composition));

      spn_context_drain(context);

      printf("COMPOSITION UNSEAL\n");

      spn(styling_unseal(styling));
      spn(styling_reset(styling));

      spn_context_drain(context);

      printf("STYLING UNSEAL\n");

      //
      // release handles
      //
      spn(path_release(context, paths, path_count));
      spn(raster_release(context, rasters, raster_count));

      //
      // free paths/rasters/layer_ids
      //
      free(paths);
      free(rasters);
      free(layer_ids);
    }

  //
  // save buffer as PPM
  //
  spn_buffer_to_ppm(surface.h.map, SPN_BUFFER_SURFACE_WIDTH, SPN_BUFFER_SURFACE_HEIGHT);

  //
  // release the builders, composition and styling
  //
  spn(path_builder_release(pb));
  spn(raster_builder_release(rb));
  spn(composition_release(composition));
  spn(styling_release(styling));

  //
  // release the transform stack
  //
  transform_stack_release(ts);

  //
  // release the context
  //
  spn(context_release(context));

  //
  // free surfaces
  //
  spn_allocator_device_perm_free(&perm_host_visible, &environment, &surface.h.dbi, surface.h.dm);
  spn_allocator_device_perm_free(&perm_device_local, &environment, &surface.d.dbi, surface.d.dm);

  //
  // dispose of allocators
  //
  spn_allocator_device_perm_dispose(&perm_host_visible, &environment);
  spn_allocator_device_perm_dispose(&perm_device_local, &environment);

  //
  // dispose of Vulkan resources
  //
  vk_ok(vk_pipeline_cache_destroy(environment.d,
                                  NULL,
                                  VK_PIPELINE_CACHE_PREFIX_STRING "vk_cache",
                                  environment.pc));

  vkDestroyDevice(environment.d, NULL);

#ifndef NDEBUG
  vkDestroyDebugReportCallbackEXT(instance, drc, NULL);
#endif

  vkDestroyInstance(instance, NULL);

  return EXIT_SUCCESS;
}

//
//
//
