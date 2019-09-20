// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
//
//

#include "vk_app_state.h"

#include <stdio.h>
#include <stdlib.h>

#include "common/macros.h"
#include "vk_assert.h"
#include "vk_cache.h"
#include "vk_debug.h"
#include "vk_shader_info_amd.h"

// For now, using GLFW to create a presentation surface on the host.
#if !VK_USE_PLATFORM_FUCHSIA
#include <GLFW/glfw3.h>

// Print GLFW errors to stderr to ease debugging.
static void
glfw_error_callback(int error, const char * message)
{
  fprintf(stderr, "GLFW:error=%d: %s\n", error, message);
}

static GLFWwindow * glfw_window;

static uint32_t glfw_window_width = 1024, glfw_window_height = 1024;

static void
glfw_setup_config(const vk_swapchain_config_t * config)
{
  if (config->window_width > 0)
    glfw_window_width = config->window_width;
  if (config->window_height > 0)
    glfw_window_height = config->window_height;
}

static GLFWwindow *
glfw_get_window()
{
  if (!glfw_window)
    {
      glfwInit();
      glfwSetErrorCallback(glfw_error_callback);
      glfw_window =
        glfwCreateWindow(glfw_window_width, glfw_window_height, "Spinel Demo Test", NULL, NULL);
      if (!glfw_window)
        {
          fprintf(stderr, "Could not create GLFW presentation window!\n");
          abort();
        }
    }
  return glfw_window;
}

static bool
glfw_poll_events()
{
  GLFWwindow * window = glfw_get_window();

  if (!window)
    return false;

  if (glfwWindowShouldClose(window))
    return false;

  glfwPollEvents();
  return true;
}

#endif  // !VK_USE_PLATFORM_FUCHSIA

//
// Define a platform-specific prefix
//

#ifdef __Fuchsia__
#define VK_PIPELINE_CACHE_PREFIX_STRING "/cache/."
#else
#define VK_PIPELINE_CACHE_PREFIX_STRING "."
#endif

// Helper macro to define a local variable pointing to a Vulkan entry point by name.
// |NAME| should be the full function name (including vk prefix).
// |LOADER| should be a macro that takes |NAME| as its parameter and returning
// the address of the function. See XXX_LOADER macros below for examples.
#define DEFINE_FUNCTION_POINTER_VAR(NAME, LOADER) PFN_##NAME NAME = (PFN_##NAME)LOADER(#NAME)

// LOADER macro used to get global function pointers.
#define GLOBAL_LOADER(name) vkGetInstanceProcAddr(NULL, name)

// LOADER macro used to get function pointers from a VkInstance named |instance|.
#define INSTANCE_LOADER(name) vkGetInstanceProcAddr(instance, name)

// LOADER macro used to get function pointers from a VkDevice named |device|.
// Assumes vkGetDeviceProcAddr exists and can be used.
#define DEVICE_LOADER(name) vkGetDeviceProcAddr(device, name)

#define GET_GLOBAL_PROC_ADDR(name) DEFINE_FUNCTION_POINTER_VAR(name, GLOBAL_LOADER)
#define GET_INSTANCE_PROC_ADDR(name) DEFINE_FUNCTION_POINTER_VAR(name, INSTANCE_LOADER)
#define GET_DEVICE_PROC_ADDR(name) DEFINE_FUNCTION_POINTER_VAR(name, DEVICE_LOADER)

//
// Instance specific info
//

typedef struct
{
  uint32_t                layers_count;
  uint32_t                extensions_count;
  VkLayerProperties *     layers;
  VkExtensionProperties * extensions;

#if VK_USE_PLATFORM_FUCHSIA
  uint32_t                swapchain_extensions_count;
  VkExtensionProperties * swapchain_extensions;
#endif

} InstanceInfo;

static InstanceInfo
instance_info_create(void)
{
  GET_GLOBAL_PROC_ADDR(vkEnumerateInstanceLayerProperties);
  GET_GLOBAL_PROC_ADDR(vkEnumerateInstanceExtensionProperties);

  InstanceInfo info = {};

  vk(EnumerateInstanceLayerProperties(&info.layers_count, NULL));
  info.layers = calloc(info.layers_count, sizeof(info.layers[0]));
  vk(EnumerateInstanceLayerProperties(&info.layers_count, info.layers));

  vk(EnumerateInstanceExtensionProperties(NULL, &info.extensions_count, NULL));
  info.extensions = calloc(info.extensions_count, sizeof(info.extensions[0]));
  vk(EnumerateInstanceExtensionProperties(NULL, &info.extensions_count, info.extensions));

#if VK_USE_PLATFORM_FUCHSIA
  vk(EnumerateInstanceExtensionProperties("VK_LAYER_FUCHSIA_imagepipe_swapchain_fb",
                                          &info.swapchain_extensions_count,
                                          NULL));
  info.swapchain_extensions =
    calloc(info.swapchain_extensions_count, sizeof(info.swapchain_extensions[0]));
  vk(EnumerateInstanceExtensionProperties("VK_LAYER_FUCHSIA_imagepipe_swapchain_fb",
                                          &info.swapchain_extensions_count,
                                          info.swapchain_extensions));
#endif
  return info;
}

static void
instance_info_destroy(InstanceInfo * info)
{
  if (info->extensions_count > 0)
    {
      free(info->extensions);
      info->extensions       = NULL;
      info->extensions_count = 0;
    }

  if (info->layers_count > 0)
    {
      free(info->layers);
      info->layers       = NULL;
      info->layers_count = 0;
    }

#if VK_USE_PLATFORM_FUCHSIA
  if (info->swapchain_extensions_count > 0)
    {
      free(info->swapchain_extensions);
      info->swapchain_extensions       = NULL;
      info->swapchain_extensions_count = 0;
    }
#endif
}

static void
instance_info_print(const InstanceInfo * info)
{
  printf("Instance info:\n");
  for (uint32_t n = 0; n < info->layers_count; ++n)
    {
      printf("  layer %s (spec version %u)\n",
             info->layers[n].layerName,
             info->layers[n].specVersion);
    }

  for (uint32_t n = 0; n < info->extensions_count; ++n)
    {
      printf("  extension %s (spec version %u)\n",
             info->extensions[n].extensionName,
             info->extensions[n].specVersion);
    }

#if VK_USE_PLATFORM_FUCHSIA
  for (uint32_t n = 0; n < info->swapchain_extensions_count; ++n)
    {
      printf("  swapchain extension %s (spec version %u)\n",
             info->swapchain_extensions[n].extensionName,
             info->swapchain_extensions[n].specVersion);
    }
#endif
}

//
// Surface presentation info
//

typedef struct
{
  uint32_t present_modes_count;
  uint32_t formats_count;

  VkSurfaceCapabilitiesKHR capabilities;
  VkPresentModeKHR *       present_modes;
  VkSurfaceFormatKHR *     formats;
} SurfaceInfo;

static SurfaceInfo
surface_info_create(VkPhysicalDevice physical_device, VkSurfaceKHR surface, VkInstance instance)
{
  DEFINE_FUNCTION_POINTER_VAR(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, INSTANCE_LOADER);
  DEFINE_FUNCTION_POINTER_VAR(vkGetPhysicalDeviceSurfaceFormatsKHR, INSTANCE_LOADER);
  DEFINE_FUNCTION_POINTER_VAR(vkGetPhysicalDeviceSurfacePresentModesKHR, INSTANCE_LOADER);

  SurfaceInfo info = {};

  // Get capabilities.
  vk(GetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &info.capabilities));

  // Get formats.
  vk(GetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &info.formats_count, NULL));
  info.formats = calloc(info.formats_count, sizeof(info.formats[0]));
  vk(GetPhysicalDeviceSurfaceFormatsKHR(physical_device,
                                        surface,
                                        &info.formats_count,
                                        info.formats));

  // Get present modes.
  vk(GetPhysicalDeviceSurfacePresentModesKHR(physical_device,
                                             surface,
                                             &info.present_modes_count,
                                             NULL));
  info.present_modes = calloc(info.present_modes_count, sizeof(info.present_modes[0]));
  vk(GetPhysicalDeviceSurfacePresentModesKHR(physical_device,
                                             surface,
                                             &info.present_modes_count,
                                             info.present_modes));
  return info;
}

static void
surface_info_destroy(SurfaceInfo * info)
{
  free(info->formats);
  info->formats       = NULL;
  info->formats_count = 0;

  free(info->present_modes);
  info->present_modes       = NULL;
  info->present_modes_count = 0;
}

//
// Platform-specific surface creation code.
//

VkResult
create_surface_khr(VkInstance                    instance,
                   const VkAllocationCallbacks * ac,
                   VkSurfaceKHR *                surface_khr)
{
#define INSTANCE_LOADER(name) vkGetInstanceProcAddr(instance, name)

#if VK_USE_PLATFORM_FUCHSIA
  DEFINE_FUNCTION_POINTER_VAR(vkCreateImagePipeSurfaceFUCHSIA, INSTANCE_LOADER);
  if (!vkCreateImagePipeSurfaceFUCHSIA)
    {
      fprintf(stderr, "MISSING %s!\n", "vkCreateImagePipeSurfaceFUCHSIA");
      return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

  VkImagePipeSurfaceCreateInfoFUCHSIA const surface_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGEPIPE_SURFACE_CREATE_INFO_FUCHSIA,
    .pNext = NULL,
  };
  return vkCreateImagePipeSurfaceFUCHSIA(instance, &surface_info, ac, surface_khr);
#else   // !FUCHSIA
  // TODO(digit): Add XCB Surface support on Linux to run presentation tests on the host.
  GLFWwindow * window = glfw_get_window();
  return glfwCreateWindowSurface(instance, window, ac, surface_khr);
#endif  // !FUCHSIA
}

static bool
physical_device_supports_presentation(VkPhysicalDevice physical_device,
                                      uint32_t         queue_family_index,
                                      VkInstance       instance)
{
#if VK_USE_PLATFORM_FUCHSIA
  return true;
#else
  return glfwGetPhysicalDevicePresentationSupport(instance, physical_device, queue_family_index) ==
         GLFW_TRUE;
#endif
}

static bool
physical_device_supports_surface(VkPhysicalDevice physical_device,
                                 uint32_t         queue_family_index,
                                 VkSurfaceKHR     surface_khr)
{
  VkBool32 supported = 0;
  VkResult result    = vkGetPhysicalDeviceSurfaceSupportKHR(physical_device,
                                                         queue_family_index,
                                                         surface_khr,
                                                         &supported);
  return (result == VK_SUCCESS) && supported;
}

//
// Swapchain setup.
//

static VkResult
vk_swapchain_state_init_surface(vk_swapchain_state_t *        state,
                                VkInstance                    instance,
                                const VkAllocationCallbacks * ac,
                                VkDevice                      device)
{
  *state = (vk_swapchain_state_t){};
  return create_surface_khr(instance, ac, &state->surface_khr);
}

static void
vk_swapchain_state_setup(vk_swapchain_state_t *        state,
                         VkInstance                    instance,
                         const VkAllocationCallbacks * ac,
                         VkPhysicalDevice              physical_device,
                         uint32_t                      graphics_queue_family_index,
                         uint32_t                      present_queue_family_index,
                         VkDevice                      device)
{
  // Sanity check.
  if (!physical_device_supports_surface(physical_device,
                                        present_queue_family_index,
                                        state->surface_khr))
    {
      fprintf(stderr, "ERROR: This device does not support presenting to this surface!\n");
      abort();
    }

  // Grab callbacks.
  GET_DEVICE_PROC_ADDR(vkCreateSwapchainKHR);
  GET_DEVICE_PROC_ADDR(vkGetSwapchainImagesKHR);

  GET_DEVICE_PROC_ADDR(vkAcquireNextImageKHR);
  GET_DEVICE_PROC_ADDR(vkQueuePresentKHR);

  state->callbacks.vkAcquireNextImageKHR = vkAcquireNextImageKHR;
  state->callbacks.vkQueuePresentKHR     = vkQueuePresentKHR;

  // Grab surface info
  SurfaceInfo surface_info = surface_info_create(physical_device, state->surface_khr, instance);

  // TODO(digit): Query surface_info for surface formats and presentation
  // modes. For now, they will be hard-coded here for triple-buffering of
  // 32-bit BRGA images with sRGB color space.
  //
  // Note that creating/destroying the SurfaceInfo above removes some
  // annoying validation layer messages, hence why they are kept here.

  state->surface_format = (VkSurfaceFormatKHR){
    .format     = VK_FORMAT_B8G8R8A8_UNORM,
    .colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
  };

  state->present_mode          = VK_PRESENT_MODE_FIFO_KHR;
  uint32_t surface_image_count = 3;

  // TODO(digit): Determine extent in a better way for windowed mode.
  state->extent = surface_info.capabilities.currentExtent;

  surface_info_destroy(&surface_info);

  VkSwapchainCreateInfoKHR const swapchain_info = {
    .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    .flags                 = 0,
    .pNext                 = NULL,
    .surface               = state->surface_khr,
    .minImageCount         = surface_image_count,
    .imageFormat           = state->surface_format.format,
    .imageColorSpace       = state->surface_format.colorSpace,
    .imageExtent           = state->extent,
    .imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .preTransform          = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    .compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    .imageArrayLayers      = 1,
    .imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = 0,
    .pQueueFamilyIndices   = NULL,
    .presentMode           = state->present_mode,
    .oldSwapchain          = VK_NULL_HANDLE,
    .clipped               = VK_FALSE,
  };
  vk(CreateSwapchainKHR(device, &swapchain_info, ac, &state->swapchain_khr));

  state->image_count = 0;
  vk(GetSwapchainImagesKHR(device, state->swapchain_khr, &state->image_count, NULL));

  if (state->image_count == 0)
    {
      fprintf(stderr, "ERROR: Could not create swapchain images!\n");
      abort();
    }
  uint32_t max_image_count = ARRAY_LENGTH_MACRO(state->images);
  if (state->image_count > max_image_count)
    {
      fprintf(stderr,
              "ERROR: Too many swapchain images (%d, should be <= %d)\n",
              state->image_count,
              max_image_count);
      abort();
    }

  vk(GetSwapchainImagesKHR(device, state->swapchain_khr, &state->image_count, state->images));

  vkGetDeviceQueue(device, graphics_queue_family_index, 0, &state->graphics_queue);
  vkGetDeviceQueue(device, present_queue_family_index, 0, &state->present_queue);
}

static void
vk_swapchain_state_destroy(vk_swapchain_state_t *        state,
                           VkDevice                      device,
                           const VkAllocationCallbacks * ac)
{
  for (uint32_t n = 0; n < state->image_count; ++n)
    {
      state->images[n] = VK_NULL_HANDLE;
    }
  state->image_count = 0;

  if (state->swapchain_khr != VK_NULL_HANDLE)
    {
      DEFINE_FUNCTION_POINTER_VAR(vkDestroySwapchainKHR, DEVICE_LOADER);
      vkDestroySwapchainKHR(device, state->swapchain_khr, ac);
      state->swapchain_khr = VK_NULL_HANDLE;
    }
}

//
//  Presentation frame specific data.
//

static vk_frame_data_t
vk_frame_data_create(VkImage                       frame_image,
                     VkFormat                      surface_format,
                     VkExtent2D                    surface_extent,
                     VkRenderPass                  render_pass,
                     VkDevice                      device,
                     const VkAllocationCallbacks * allocator)
{
  vk_frame_data_t frame_data = {};

  const VkImageViewCreateInfo image_view_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .pNext = NULL,
    .image = frame_image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = surface_format,
    .components = {
      .r = VK_COMPONENT_SWIZZLE_IDENTITY,
      .g = VK_COMPONENT_SWIZZLE_IDENTITY,
      .b = VK_COMPONENT_SWIZZLE_IDENTITY,
      .a = VK_COMPONENT_SWIZZLE_IDENTITY,
    },
    .subresourceRange = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
  };
  vk(CreateImageView(device, &image_view_info, allocator, &frame_data.image_view));

  const VkSemaphoreCreateInfo sem_info = {
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
  };

  const VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };

  vk(CreateSemaphore(device, &sem_info, allocator, &frame_data.available_semaphore));
  vk(CreateSemaphore(device, &sem_info, allocator, &frame_data.rendered_semaphore));
  vk(CreateFence(device, &fence_info, allocator, &frame_data.inflight_fence));

  const VkFramebufferCreateInfo framebufferInfo = {
    .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
    .renderPass      = render_pass,
    .attachmentCount = 1,
    .pAttachments    = &frame_data.image_view,
    .width           = surface_extent.width,
    .height          = surface_extent.height,
    .layers          = 1,
  };
  vk(CreateFramebuffer(device, &framebufferInfo, allocator, &frame_data.framebuffer));

  return frame_data;
}

static void
vk_frame_data_destroy(vk_frame_data_t *             frame_data,
                      VkDevice                      device,
                      const VkAllocationCallbacks * allocator)
{
  vkDestroyFramebuffer(device, frame_data->framebuffer, allocator);
  vkDestroyFence(device, frame_data->inflight_fence, allocator);
  vkDestroySemaphore(device, frame_data->rendered_semaphore, allocator);
  vkDestroySemaphore(device, frame_data->available_semaphore, allocator);

  vkDestroyImageView(device, frame_data->image_view, allocator);
}

static vk_presentation_frames_t
vk_presentation_frames_create(uint32_t                      queue_family_index,
                              uint32_t                      frame_count,
                              const VkImage *               frame_images,
                              VkFormat                      surface_format,
                              VkExtent2D                    surface_extent,
                              VkRenderPass                  render_pass,
                              VkDevice                      device,
                              const VkAllocationCallbacks * allocator)
{
  vk_presentation_frames_t frames = {
    .count         = frame_count,
    .current_frame = 0,
  };

  const VkCommandPoolCreateInfo poolInfo = {
    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = queue_family_index,
    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
  };

  vk(CreateCommandPool(device, &poolInfo, allocator, &frames.command_pool));

  const VkCommandBufferAllocateInfo allocInfo = {
    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool        = frames.command_pool,
    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = frame_count,
  };

  vk(AllocateCommandBuffers(device, &allocInfo, frames.command_buffers));

  for (uint32_t n = 0; n < frame_count; ++n)
    {
      frames.data[n] = vk_frame_data_create(frame_images[n],
                                            surface_format,
                                            surface_extent,
                                            render_pass,
                                            device,
                                            allocator);
    }

  return frames;
}

static void
vk_presentation_frames_destroy(vk_presentation_frames_t *    frames,
                               VkDevice                      device,
                               const VkAllocationCallbacks * allocator)
{
  if (frames->count > 0)
    {
      for (uint32_t n = 0; n < frames->count; ++n)
        {
          vk_frame_data_destroy(&frames->data[n], device, allocator);
        }

      vkFreeCommandBuffers(device, frames->command_pool, frames->count, frames->command_buffers);

      vkDestroyCommandPool(device, frames->command_pool, allocator);

      frames->count = 0;
    }
}

static void
vk_presentation_frames_prepare_image(vk_presentation_frames_t * frames,
                                     VkDevice                   device,
                                     uint32_t *                 image_index)
{
  uint32_t          current_frame = frames->current_frame;
  vk_frame_data_t * frame_data    = &frames->data[current_frame];

  //printf("WAIT_IDLE!"); fflush(stdout);
  //vkDeviceWaitIdle(device);

  // Wait for the frame's fence to ensure that its command buffer has
  // completed and can be reused.
  VkFence inflight_fence = frame_data->inflight_fence;

  //printf("WAIT_FENCE(f%u)!", current_frame); fflush(stdout);
  vkWaitForFences(device, 1, &inflight_fence, VK_TRUE, UINT64_MAX);
  vkResetFences(device, 1, &inflight_fence);
}

static void
vk_presentation_frames_record_image(vk_presentation_frames_t * frames, uint32_t image_index)
{
  frames->image_index = image_index;
}

bool
vk_app_state_init(vk_app_state_t * app_state, const vk_app_state_config_t * config)
{
  *app_state = (const vk_app_state_t){};

  if (config->enable_debug_report)
    {
      // For debugging only!
      InstanceInfo instance_info = instance_info_create();
      instance_info_print(&instance_info);
      instance_info_destroy(&instance_info);
    }

  //
  // create a Vulkan instances
  //
  VkApplicationInfo const app_info = {

    .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pNext              = NULL,
    .pApplicationName   = config->app_name ? config->app_name : "VK Test",
    .applicationVersion = 0,
    .pEngineName        = "Graphics Compute VK",
    .engineVersion      = 0,
    .apiVersion         = VK_API_VERSION_1_1
  };

  char const * enabled_layers[8] = {};
  uint32_t     num_layers        = 0;

  char const * enabled_extensions[8] = {};
  uint32_t     num_extensions        = 0;

  if (config->enable_validation)
    {
      enabled_layers[num_layers++] = "VK_LAYER_LUNARG_standard_validation";
    }

  if (config->enable_debug_report)
    {
      enabled_extensions[num_extensions++] = VK_EXT_DEBUG_REPORT_EXTENSION_NAME;
    }

  bool enable_swapchain = config->swapchain_config != NULL;
  if (enable_swapchain)
    {
      enabled_extensions[num_extensions++] = VK_KHR_SURFACE_EXTENSION_NAME;

      // NOTE: On Fuchsia, swapchain extensions are provided by a layer.
      // For now, only use the layer allowing presenting to the framebuffer
      // directly (another layer is provided to display in a window, but this one
      // is far more work to get everything working).
#if VK_USE_PLATFORM_FUCHSIA
      enabled_layers[num_layers++]         = "VK_LAYER_FUCHSIA_imagepipe_swapchain_fb";
      enabled_extensions[num_extensions++] = VK_FUCHSIA_IMAGEPIPE_SURFACE_EXTENSION_NAME;
#else
      glfwInit();
      uint32_t      glfw_extensions_count = 0;
      const char ** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extensions_count);
      for (uint32_t n = 0; n < glfw_extensions_count; ++n)
        {
          enabled_extensions[num_extensions++] = glfw_extensions[n];
        }
#endif
    }

  VkInstanceCreateInfo const instance_info = {

    .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pNext                   = NULL,
    .flags                   = 0,
    .pApplicationInfo        = &app_info,
    .enabledLayerCount       = num_layers,
    .ppEnabledLayerNames     = enabled_layers,
    .enabledExtensionCount   = num_extensions,
    .ppEnabledExtensionNames = enabled_extensions,
  };

  vk(CreateInstance(&instance_info, NULL, &app_state->instance));
  VkInstance instance = app_state->instance;

  //
  //
  //
  if (config->enable_debug_report)
    {
      GET_INSTANCE_PROC_ADDR(vkCreateDebugReportCallbackEXT);
      if (!vkCreateDebugReportCallbackEXT)
        {
          fprintf(stderr, "WARNING: vkCreateDebugReportCallbackEXT not supported by Vulkan ICD!");
        }
      else
        {
          VkDebugReportFlagsEXT const drf =
            VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
            VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT |
            VK_DEBUG_REPORT_DEBUG_BIT_EXT;

          struct VkDebugReportCallbackCreateInfoEXT const drcci = {

            .sType       = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
            .pNext       = NULL,
            .flags       = drf,
            .pfnCallback = vk_debug_report_cb,
            .pUserData   = NULL
          };

          vk(CreateDebugReportCallbackEXT(app_state->instance, &drcci, NULL, &app_state->drc));
          app_state->has_debug_report = true;
        }
    }

  //
  // Prepare Vulkan environment for Spinel
  //
  app_state->d   = VK_NULL_HANDLE;
  app_state->ac  = NULL;
  app_state->pc  = VK_NULL_HANDLE;
  app_state->pd  = VK_NULL_HANDLE;
  app_state->qfi = 0;

  //
  // acquire all physical devices
  //
  uint32_t pd_count;

  vk(EnumeratePhysicalDevices(app_state->instance, &pd_count, NULL));
  if (pd_count == 0)
    {
      fprintf(stderr, "No device found\n");
      return false;
    }

  VkPhysicalDevice * pds = malloc(pd_count * sizeof(*pds));
  vk(EnumeratePhysicalDevices(app_state->instance, &pd_count, pds));

  //
  // select the first device if *both* ids aren't provided
  //
  VkPhysicalDeviceProperties pdp;
  vkGetPhysicalDeviceProperties(pds[0], &app_state->pdp);

  uint32_t vendor_id = config->vendor_id ? config->vendor_id : app_state->pdp.vendorID;
  uint32_t device_id = config->device_id ? config->device_id : app_state->pdp.deviceID;

  //
  // list all devices
  //
  app_state->pd = VK_NULL_HANDLE;

  for (uint32_t ii = 0; ii < pd_count; ii++)
    {
      VkPhysicalDeviceProperties pdp_tmp;

      vkGetPhysicalDeviceProperties(pds[ii], &pdp_tmp);

      bool const is_match = (pdp_tmp.vendorID == vendor_id) && (pdp_tmp.deviceID == device_id);

      if (is_match)
        {
          pdp           = pdp_tmp;
          app_state->pd = pds[ii];
        }

      fprintf(stdout,
              "%c %X : %X : %s\n",
              is_match ? '*' : ' ',
              pdp_tmp.vendorID,
              pdp_tmp.deviceID,
              pdp_tmp.deviceName);
    }

  if (app_state->pd == VK_NULL_HANDLE)
    {
      fprintf(stderr, "Device %X : %X not found.\n", vendor_id, device_id);

      return EXIT_FAILURE;
    }

  free(pds);

  //
  // get the physical device's memory props
  //
  vkGetPhysicalDeviceMemoryProperties(app_state->pd, &app_state->pdmp);

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
  uint32_t qfc = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(app_state->pd, &qfc, NULL);
  VkQueueFamilyProperties qfp[qfc];
  vkGetPhysicalDeviceQueueFamilyProperties(app_state->pd, &qfc, qfp);

  // TODO(digit): Actually probe the queue families to find the best one.
  // For now, broadly assume the first one always works.
  app_state->qfi = 0;

  // Sanity checks, just in case!
  VkQueueFlags wantedFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
  if ((qfp[app_state->qfi].queueFlags & wantedFlags) != wantedFlags)
    {
      fprintf(stderr, "This device has not queue with graphics & compute support!\n");
      return false;
    }

  if (enable_swapchain)
    {
      if (!physical_device_supports_presentation(app_state->pd,
                                                 app_state->qfi,
                                                 app_state->instance))
        {
          fprintf(stderr, "This device does not support presentation/display!\n");
          return false;
        }

      // If swapchain presentation is enabled, a surface need to be created first
      // in order to select the right device / family queues that can actually
      // present to it.
      VkResult result = vk_swapchain_state_init_surface(&app_state->swapchain_state,
                                                        app_state->instance,
                                                        app_state->ac,
                                                        app_state->d);
      if (result != VK_SUCCESS)
        {
          fprintf(stderr, "Could not create display surface: %s\n", vk_get_result_string(result));
          return false;
        }

      if (!physical_device_supports_surface(app_state->pd,
                                            app_state->qfi,
                                            app_state->swapchain_state.surface_khr))
        {
          fprintf(stderr, "This device does not support presentation to this surface!\n");
          vk_swapchain_state_destroy(&app_state->swapchain_state, app_state->d, app_state->ac);
          return false;
        }
    }

  //
  // create queue
  //
  float const qp[] = { 1.0f };

  VkDeviceQueueCreateInfo const queue_info = {

    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .pNext            = NULL,
    .flags            = 0,
    .queueFamilyIndex = app_state->qfi,
    .queueCount       = 1,
    .pQueuePriorities = qp
  };

  const char * device_extensions[8]  = {};
  uint32_t     num_device_extensions = 0;

  //
  // Enable AMD GCN shader info extension
  //
  if (config->enable_amd_statistics && app_state->pdp.vendorID == 0x1002)
    {
      device_extensions[num_device_extensions++] = VK_AMD_SHADER_INFO_EXTENSION_NAME;
      vk_shader_info_amd_statistics_set_enabled(true);
    }

  //
  // Enable swapchain device extension if needed.
  //
  if (enable_swapchain)
    {
      device_extensions[num_device_extensions++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }

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
    .pQueueCreateInfos       = &queue_info,
    .enabledLayerCount       = 0,
    .ppEnabledLayerNames     = NULL,
    .enabledExtensionCount   = num_device_extensions,
    .ppEnabledExtensionNames = device_extensions,
    .pEnabledFeatures        = &device_features
  };

  vk(CreateDevice(app_state->pd, &device_info, app_state->ac, &app_state->d));

  if (enable_swapchain)
    {
#if !VK_USE_PLATFORM_FUCHSIA
      glfw_setup_config(config->swapchain_config);
#endif
      vk_swapchain_state_setup(&app_state->swapchain_state,
                               app_state->instance,
                               app_state->ac,
                               app_state->pd,
                               app_state->qfi,  // graphics
                               app_state->qfi,  // present
                               app_state->d);
    }

  //
  // create the pipeline cache
  //
  vk(_pipeline_cache_create(app_state->d,
                            NULL,
                            VK_PIPELINE_CACHE_PREFIX_STRING "vk_cache",
                            &app_state->pc));

  return true;
}

void
vk_app_state_destroy(vk_app_state_t * app_state)
{
  vk(_pipeline_cache_destroy(app_state->d,
                             NULL,
                             VK_PIPELINE_CACHE_PREFIX_STRING "vk_cache",
                             app_state->pc));

  vk_presentation_frames_destroy(&app_state->presentation_frames, app_state->d, app_state->ac);
  vk_swapchain_state_destroy(&app_state->swapchain_state, app_state->d, app_state->ac);

  vkDestroyDevice(app_state->d, NULL);

  if (app_state->has_debug_report)
    {
      VkInstance instance = app_state->instance;
      GET_INSTANCE_PROC_ADDR(vkDestroyDebugReportCallbackEXT);
      vkDestroyDebugReportCallbackEXT(app_state->instance, app_state->drc, NULL);
    }

  vkDestroyInstance(app_state->instance, NULL);
}

bool
vk_app_state_poll_events(vk_app_state_t * app_state)
{
#if VK_USE_PLATFORM_FUCHSIA
  // TODO(digit): Find a way to receive events from the system.
  return true;
#else
  return glfw_poll_events();
#endif
}

void
vk_app_state_init_presentation(vk_app_state_t * app_state, VkRenderPass render_pass)
{
  // Sanity check that swapchain was properly enabled.
  if (app_state->swapchain_state.swapchain_khr == VK_NULL_HANDLE)
    {
      fprintf(stderr, "ERROR: swapchain support was not enabled!\n");
      abort();
    }

  app_state->presentation_frames =
    vk_presentation_frames_create(app_state->qfi,
                                  app_state->swapchain_state.image_count,
                                  app_state->swapchain_state.images,
                                  app_state->swapchain_state.surface_format.format,
                                  app_state->swapchain_state.extent,
                                  render_pass,
                                  app_state->d,
                                  app_state->ac);
}

vk_frame_data_t *
vk_app_state_get_image_frame_data(vk_app_state_t * app_state, uint32_t image_index)
{
  return &app_state->presentation_frames.data[image_index];
}

VkCommandBuffer
vk_app_state_get_image_command_buffer(const vk_app_state_t * app_state, uint32_t image_index)
{
  return app_state->presentation_frames.command_buffers[image_index];
}

bool
vk_app_state_prepare_next_image(vk_app_state_t *  app_state,
                                uint32_t *        image_index,
                                VkCommandBuffer * command_buffer)
{
  vk_presentation_frames_t * frames = &app_state->presentation_frames;
  vk_presentation_frames_prepare_image(frames, app_state->d, image_index);

  uint32_t                current_frame = frames->current_frame;
  const vk_frame_data_t * frame_data    = &frames->data[current_frame];

  VkResult result = app_state->swapchain_state.callbacks.vkAcquireNextImageKHR(
    app_state->d,
    app_state->swapchain_state.swapchain_khr,
    UINT64_MAX,
    frame_data->available_semaphore,
    VK_NULL_HANDLE,  // no fence to signal.
    image_index);

  switch (result)
    {
      case VK_SUCCESS:
      case VK_SUBOPTIMAL_KHR:
        break;
      case VK_ERROR_OUT_OF_DATE_KHR:
        *image_index = ~0U;
        return false;
      default:
        fprintf(stderr,
                "ERROR: Could not acquire next swapchain image: %s\n",
                vk_get_result_string(result));
        abort();
    }

  vk_presentation_frames_record_image(frames, *image_index);
  if (command_buffer)
    {
      *command_buffer = frames->command_buffers[*image_index];
    }
  return true;
}

void
vk_app_state_submit_image(vk_app_state_t * app_state)
{
  const vk_presentation_frames_t * frames     = &app_state->presentation_frames;
  const vk_frame_data_t *          frame_data = &frames->data[frames->current_frame];

  VkCommandBuffer command_buffer = frames->command_buffers[frames->image_index];

  // Wait on a single semaphore for now.
  const VkPipelineStageFlags waitStages[] = {
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
  };

  VkSemaphore waitSemaphores[] = {
    frame_data->available_semaphore,
  };

  const VkSubmitInfo submitInfo = {
    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .waitSemaphoreCount   = 1,
    .pWaitSemaphores      = waitSemaphores,
    .pWaitDstStageMask    = waitStages,
    .commandBufferCount   = 1,
    .pCommandBuffers      = &command_buffer,
    .signalSemaphoreCount = 1,
    .pSignalSemaphores    = &frame_data->rendered_semaphore,
  };

  //printf("SUBMIT(f%u,i%u)!", current_frame, image_index); fflush(stdout);
  vk(QueueSubmit(app_state->swapchain_state.graphics_queue,
                 1,
                 &submitInfo,
                 frame_data->inflight_fence));
}

bool
vk_app_state_present_image(vk_app_state_t * app_state, uint32_t image_index)
{
  vk_presentation_frames_t * frames     = &app_state->presentation_frames;
  const vk_frame_data_t *    frame_data = &frames->data[frames->current_frame];

  const VkPresentInfoKHR presentInfo = {
    .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores    = &frame_data->rendered_semaphore,
    .swapchainCount     = 1,
    .pSwapchains        = &app_state->swapchain_state.swapchain_khr,
    .pImageIndices      = &image_index,
  };

  VkResult result =
    app_state->swapchain_state.callbacks.vkQueuePresentKHR(app_state->swapchain_state.present_queue,
                                                           &presentInfo);
  switch (result)
    {
      case VK_SUCCESS:
        break;
      case VK_ERROR_OUT_OF_DATE_KHR:
      case VK_SUBOPTIMAL_KHR:
        return false;
      default:
        fprintf(stderr, "ERROR: Problem during presentation: %s\n", vk_get_result_string(result));
        abort();
    }

  frames->current_frame = (frames->current_frame + 1) % frames->count;
  return true;
}

void
vk_app_state_destroy_presentation(vk_app_state_t * app_state)
{
  vk_presentation_frames_destroy(&app_state->presentation_frames, app_state->d, app_state->ac);
}

// Convert physical device type to a string.
static const char *
deviceTypeString(VkPhysicalDeviceType device_type)
{
  switch (device_type)
    {
      case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        return "OTHER";
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "INTEGRATED_GPU";
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "DISCRETE_GPU";
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "VIRTUAL_GPU";
      case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "CPU";
      default:
        return "UNKNOWN_VALUE";
    }
}

static void
size_to_string(size_t size, char * buffer, size_t buffer_size)
{
  if (size < 65536)
    {
      snprintf(buffer, buffer_size, "%u", (unsigned)size);
    }
  else if (size < 1024 * 1024)
    {
      snprintf(buffer, buffer_size, "%.1f kiB", size / 1024.);
    }
  else if (size < 1024 * 1024 * 1024)
    {
      snprintf(buffer, buffer_size, "%.1f MiB", size / (1024. * 1024.));
    }
  else
    {
      snprintf(buffer, buffer_size, "%1.f GiB", size / (1024. * 1024. * 1024.));
    }
}

void
vk_app_state_print(const vk_app_state_t * app_state)
{
  const uint32_t vendor_id = app_state->pdp.vendorID;
  const uint32_t device_id = app_state->pdp.deviceID;

  printf("Device (vendor_id, device_id)=(0x%X, 0x%0X)\n", vendor_id, device_id);
  printf("  VkInstance:            %p\n", app_state->instance);
  printf("  Allocation callbacks:  %p\n", app_state->ac);
  printf("  VkPhysicalDevice:      %p\n", app_state->pd);
  printf("  VkDevice:              %p\n", app_state->d);

  printf("  VkPhysicalDeviceProperties:\n");
  printf("     apiVersion:       0x%x\n", app_state->pdp.apiVersion);
  printf("     driverVersion:    0x%x\n", app_state->pdp.driverVersion);
  printf("     vendorID:         0x%x\n", app_state->pdp.vendorID);
  printf("     deviceID:         0x%x\n", app_state->pdp.deviceID);
  printf("     deviceType:       %s\n", deviceTypeString(app_state->pdp.deviceType));
  printf("     deviceName:       %s\n", app_state->pdp.deviceName);

  printf("  VkPhysicalDeviceMemoryProperties:\n");
  for (uint32_t n = 0; n < app_state->pdmp.memoryHeapCount; ++n)
    {
      uint32_t flags = app_state->pdmp.memoryHeaps[n].flags;
      char     size_str[32];
      size_to_string(app_state->pdmp.memoryHeaps[n].size, size_str, sizeof(size_str));
      printf("      heap index=%-2d size=%-8s flags=0x%08x", n, size_str, flags);
#define FLAG_BIT(name)                                                                             \
  do                                                                                               \
    {                                                                                              \
      if (flags & VK_MEMORY_HEAP_##name##_BIT)                                                     \
        printf(" " #name);                                                                         \
    }                                                                                              \
  while (0)
      FLAG_BIT(DEVICE_LOCAL);
      FLAG_BIT(MULTI_INSTANCE);
#undef FLAG_BIT
      printf("\n");
    }
  for (uint32_t n = 0; n < app_state->pdmp.memoryTypeCount; ++n)
    {
      uint32_t flags = app_state->pdmp.memoryTypes[n].propertyFlags;
      printf("      type index=%-2d heap=%-2d flags=0x%08x",
             n,
             app_state->pdmp.memoryTypes[n].heapIndex,
             flags);
#define FLAG_BIT(name)                                                                             \
  do                                                                                               \
    {                                                                                              \
      if (flags & VK_MEMORY_PROPERTY_##name##_BIT)                                                 \
        printf(" " #name);                                                                         \
    }                                                                                              \
  while (0)
      FLAG_BIT(DEVICE_LOCAL);
      FLAG_BIT(HOST_VISIBLE);
      FLAG_BIT(HOST_COHERENT);
      FLAG_BIT(HOST_CACHED);
      FLAG_BIT(LAZILY_ALLOCATED);
      FLAG_BIT(PROTECTED);
#undef FLAG_BIT
      printf("\n");
    }

  printf("  has_debug_report:     %s\n", app_state->has_debug_report ? "true" : "false");

  const vk_swapchain_state_t * swapchain = &app_state->swapchain_state;
  if (swapchain->surface_khr != VK_NULL_HANDLE)
    {
      printf("  Swapchain state:\n");
      printf("    VkSurfaceKHR:       %p\n", swapchain->surface_khr);
      printf("    VkSwapchainKHR:     %p\n", swapchain->swapchain_khr);
      printf("    Graphics queue:     %p\n", swapchain->graphics_queue);
      printf("    Present queue:      %p\n", swapchain->present_queue);
      printf("    Extent:             %dx%d\n", swapchain->extent.width, swapchain->extent.height);
      printf("    Image count:        %u\n", swapchain->image_count);
    }
}
