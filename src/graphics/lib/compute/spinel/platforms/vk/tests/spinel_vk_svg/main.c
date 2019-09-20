// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <float.h>
#include <stdio.h>
#include <stdlib.h>

#include "allocator_device.h"
#include "common/macros.h"
#include "common/vk/vk_app_state.h"
#include "common/vk/vk_assert.h"
#include "common/vk/vk_cache.h"
#include "common/vk/vk_debug.h"
#include "ext/color/color.h"
#include "ext/transform_stack/transform_stack.h"
#include "spinel/spinel_assert.h"
#include "spinel/spinel_vk.h"

#include "presentation.h"
#include "triangle_shaders.h"

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
  // Setup vulkan application state.
  //

  const vk_app_state_config_t app_config = {
    .app_name = "Fuchsia Spinel/VK Test",
    .enable_validation = true,
    .enable_debug_report = true,
#if defined(SPN_VK_SHADER_INFO_AMD_STATISTICS) || defined(SPN_VK_SHADER_INFO_AMD_DISASSEMBLY)
    .enable_amd_statistics = true,
#endif
    .swapchain_config = &(const vk_swapchain_config_t){
      .window_width = SPN_BUFFER_SURFACE_WIDTH,
      .window_height = SPN_BUFFER_SURFACE_HEIGHT,
    },
  };

  vk_app_state_t app_state;
  if (!vk_app_state_init(&app_state, &app_config))
    return EXIT_FAILURE;

  //
  // Setup Spinel environment and context
  //
  struct spn_vk_environment environment = {
    .d    = app_state.d,
    .ac   = app_state.ac,
    .pc   = app_state.pc,
    .pd   = app_state.pd,
    .pdmp = app_state.pdmp,
    .qfi  = app_state.qfi,
  };

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

  if (!spn_vk_find_target(app_state.pdp.vendorID,
                          app_state.pdp.deviceID,
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

  ////////////////////////////////////
  //
  // PRESENTATION AND GRAPHICS PIPELINE SETUP
  //
  VkDevice                     device = app_state.d;
  const VkAllocationCallbacks* allocator = app_state.ac;
  VkSurfaceFormatKHR           surface_format = app_state.swapchain_state.surface_format;
  VkExtent2D                   surface_extent = app_state.swapchain_state.extent;

  VkRenderPass     render_pass       = create_render_pass(device, allocator, surface_format.format);
  VkPipelineLayout pipeline_layout   = create_pipeline_layout(device, allocator);
  VkPipeline       graphics_pipeline = create_graphics_pipeline(
      device, allocator, surface_extent, render_pass, pipeline_layout);

  vk_app_state_init_presentation(&app_state, render_pass);

  if (app_config.enable_debug_report)
    vk_app_state_print(&app_state);

  ////////////////////////////////////
  //
  // MAIN LOOP
  //
  uint32_t frame_counter = 0;
  while (vk_app_state_poll_events(&app_state))
  {
    uint32_t image_index;

    if (!vk_app_state_prepare_next_image(&app_state, &image_index, NULL))
      {
        // Window was resized! For now just exit!!
        // TODO(digit): Handle resize!!
        break;
      }

    // For each frame, the following steps are performed:
    //
    //  1) Invoke spinel rendering into the destination device-local buffer.
    //     This happens on the compute pipeline.
    //
    //  2) Invoke a render pass to clear the swapchain image, and draw
    //     a rectangle on it (taken from the triangle example).
    //
    //  3) Add a barrier to wait for the completion of 1) and 2) above
    //     and prepare for the buffer transfer, while changing the image's
    //     layout.
    //
    //  4) Copy the buffer content to the image.
    //
    //  5) Add a barrier to wait for the end of the transfer and change the
    //     image's layout back to presentation.
    //

    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////
    /////
    /////  STEP 1: SPINEL RENDERING THROUGH COMPUTE
    /////
    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////

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

    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////
    /////
    /////  STEP 2: GRAPHICS PIPELINE RENDERING
    /////
    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////

    VkCommandBuffer   command_buffer = vk_app_state_get_image_command_buffer(&app_state, image_index);
    vk_frame_data_t * frame_data = vk_app_state_get_image_frame_data(&app_state, image_index);
    VkImage           swapchain_image = app_state.swapchain_state.images[image_index];

    const VkCommandBufferBeginInfo beginInfo = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };
    vk(BeginCommandBuffer(command_buffer, &beginInfo));

    const VkRenderPassBeginInfo renderPassInfo = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = frame_data->framebuffer,
      .renderArea = {
          .offset = {0, 0},
          .extent = surface_extent,
      },
      .clearValueCount = 1,
      .pClearValues = &(const VkClearValue){.color = {{0.0f, 0.0f, 0.0f, 1.0f}}},
    };
    vkCmdBeginRenderPass(command_buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    {
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
      vkCmdDraw(command_buffer, 3, 1, 0, 0);  // TODO(digit): Remove this??
    }
    vkCmdEndRenderPass(command_buffer);

    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////
    /////
    /////  STEP 3: IMAGE MEMORY BARRIER + LAYOUT TRANSITION
    /////
    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,    // dependencyFlags
        0,    // memoryBarrierCount
        NULL, // pMemoryBarriers
        0,    // bufferMemoryBarrierCount
        NULL, // pBufferMemoryBarriers
        1,    // imageMemoryBarrierCount,
        &(const VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        }
    );

    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////
    /////
    /////  STEP 4: BUFFER TO IMAGE COPY
    /////
    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////

    // TODO(digit): Compute proper intersection rectangle when the
    // buffer destination rectangle is not completely contained within
    // the image. Will allow for interesting animations.

    vkCmdCopyBufferToImage(
        command_buffer,
        surface.d.dbi.buffer,                           // src buffer
        swapchain_image,                                // dst image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,           // dst image layout
        1,                                              // region count
        &(const VkBufferImageCopy){                     // regions
            .bufferOffset = 0,
            .bufferRowLength = SPN_BUFFER_SURFACE_WIDTH,
            .bufferImageHeight = SPN_BUFFER_SURFACE_HEIGHT,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {
                .x = 0,
                .y = 0,
                .z = 0,
            },
            .imageExtent = {
                .width = SPN_BUFFER_SURFACE_WIDTH,
                .height = SPN_BUFFER_SURFACE_HEIGHT,
                .depth = 1,
            },
        }
    );

    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////
    /////
    /////  STEP 5: IMAGE MEMORY BUFFER + LAYOUT TRANSITION
    /////
    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,    // dependencyFlags
        0,    // memoryBarrierCount
        NULL, // pMemoryBarriers
        0,    // bufferMemoryBarrierCount
        NULL, // pBufferMemoryBarriers
        1,    // imageMemoryBarrierCount,
        &(const VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        }
    );

    vk(EndCommandBuffer(command_buffer));

    vk_app_state_submit_image(&app_state);

    vk_app_state_present_image(&app_state, image_index);

    // Print a small tick every two seconds (assuming a 60hz swapchain) to
    // check that everything is working, even if the image is static at this
    // point.
    if (app_config.enable_debug_report && ++frame_counter == 60 * 2)
      {
        printf("!");
        fflush(stdout);
        frame_counter = 0;
      }
  }
#if 0
  //
  // save buffer as PPM
  //
  spn_buffer_to_ppm(surface.h.map, SPN_BUFFER_SURFACE_WIDTH, SPN_BUFFER_SURFACE_HEIGHT);
#endif
  //
  // release the context
  //
  spn(context_release(context));

  //
  // free graphics pipeline + presentation surface.
  //
  vkDestroyPipeline(device, graphics_pipeline, allocator);
  vkDestroyPipelineLayout(device, pipeline_layout, allocator);
  vkDestroyRenderPass(device, render_pass, allocator);

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
  vk_app_state_destroy(&app_state);

  return EXIT_SUCCESS;
}

//
//
//
