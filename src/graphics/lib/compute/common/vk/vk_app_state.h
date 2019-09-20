// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_COMPUTE_COMMON_VK_VK_APP_STATE_H_
#define SRC_GRAPHICS_LIB_COMPUTE_COMMON_VK_VK_APP_STATE_H_

//
//
//

#include <stdbool.h>
#include <vulkan/vulkan_core.h>

//
//
//

#ifdef __cplusplus
extern "C" {
#endif

//
//
//

typedef struct
{
  uint32_t window_width;
  uint32_t window_height;
} vk_swapchain_config_t;

// Structure containing configuration information for a vk_app_state_t
// instance creation.
typedef struct
{
  const char * app_name;           // Optional application name.
  bool         enable_validation;  // True to enable validation layers.
  bool enable_debug_report;        // True to enable debug report callbacks (failure is not fatal).
  bool
    enable_amd_statistics;  // True to enable AMD statistics, if available (failure is not fatal).

  // NULL, or a pointer to a vk_swapchain_config_t to enable swapchain support.
  const vk_swapchain_config_t * swapchain_config;

  uint32_t vendor_id;  // 0, or a Vulkan vendor ID.
  uint32_t device_id;  // 0, or a Vulkan device ID.

} vk_app_state_config_t;

// Structure used to hold the swapchain-related state.
typedef struct
{
  VkSurfaceKHR       surface_khr;
  VkSwapchainKHR     swapchain_khr;
  VkQueue            graphics_queue;
  VkQueue            present_queue;
  VkSurfaceFormatKHR surface_format;
  VkPresentModeKHR   present_mode;
  VkExtent2D         extent;
  uint32_t           image_count;
  VkImage            images[8];

  struct
  {
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR     vkQueuePresentKHR;
  } callbacks;

} vk_swapchain_state_t;

// Data related to a single presentation frame.
typedef struct
{
  VkImageView   image_view;
  VkFramebuffer framebuffer;
  VkSemaphore   available_semaphore;
  VkSemaphore   rendered_semaphore;
  VkFence       inflight_fence;
} vk_frame_data_t;

typedef struct
{
  uint32_t        count;
  uint32_t        current_frame;
  uint32_t        image_index;
  VkCommandPool   command_pool;
  vk_frame_data_t data[8];
  VkCommandBuffer command_buffers[8];
} vk_presentation_frames_t;

// Simple struct to gather application-specific Vulkan state for our test
// programs. Usage is:
//   1) Use vk_app_state_init() to initialize instance.
//   2) Use the handles and structs provided to perform Vulkan operations.
//   3) Call vk_app_state_destroy() to release all Vulkan resources.
typedef struct
{
  VkInstance                       instance;
  const VkAllocationCallbacks *    ac;
  VkDevice                         d;
  VkPipelineCache                  pc;
  VkPhysicalDevice                 pd;
  VkPhysicalDeviceProperties       pdp;
  VkPhysicalDeviceMemoryProperties pdmp;
  uint32_t                         qfi;  // queue family index

  // Only used when enable_swapchain is true.
  vk_swapchain_state_t     swapchain_state;
  vk_presentation_frames_t presentation_frames;

  // Used internally only.
  VkDebugReportCallbackEXT drc;
  bool                     has_debug_report;
  bool                     has_amd_statistics;

} vk_app_state_t;

// Initialize vk_app_state_t instance. |vendor_id| and |device_id| should
// be pointers to uint32_t values which can be either 0 or a specific Vulkan
// vendor or device ID. If both are zero, the first device will be selected.
// If both are non-zero, only this specific device will be selected. Otherwise
// results are undefined.
//
// Returns true on success, false on failure (after printing error message to
// stderr).
extern bool
vk_app_state_init(vk_app_state_t * app_state, const vk_app_state_config_t * app_state_config);

// Destroy a vk_app_state_t instance.
extern void
vk_app_state_destroy(vk_app_state_t * app_state);

// Dump state of a vk_app_state_t to stdout for debugging.
extern void
vk_app_state_print(const vk_app_state_t * app_state);

// Poll UI events, return true on success, false if the program should quit.
// This function should be called before any frame draw call.
extern bool
vk_app_state_poll_events(vk_app_state_t * app_state);

// Initialize presentation according to a single render pass.
// Don't forget to call vk_app_state_destroy_presentation() before destroying
// the render pass when done.
extern void
vk_app_state_init_presentation(vk_app_state_t * app_state, VkRenderPass render_pass);

// Retrieve presentation image data for |image_index|.
extern vk_frame_data_t *
vk_app_state_get_image_frame_data(vk_app_state_t * app_state, uint32_t image_index);

// Retrieve presentation image command buffer for |image_index|.
extern VkCommandBuffer
vk_app_state_get_image_command_buffer(const vk_app_state_t * app_state, uint32_t image_index);

// Retrieve a new swapchain image index.
// On success, returns true and sets |*image_index| to a valid swapchain index
// and |*command_buffer| to the corresponding command buffer.
// On failure (i.e. window resizing), return false.
extern bool
vk_app_state_prepare_next_image(vk_app_state_t *  app_state,
                                uint32_t *        image_index,
                                VkCommandBuffer * command_buffer);

// Submit a new swapchain image's command buffer to the graphics queue.
// This needs to be called at least once with the command buffer corresponding
// to the image index returned by vk_app_state_acquire_next_image()
extern void
vk_app_state_submit_image(vk_app_state_t * app_state);

// Tell the swapchain to present the image identified by |image_index| after
// waiting for |wait_semaphore|. On success, return true, false otherwise
// (i.e. if the window was resized).
extern bool
vk_app_state_present_image(vk_app_state_t * app_state, uint32_t image_index);

// Destroy presentation data. This must be called before releasing the render
// pass that was sent tp vk_app_state_init_presentation().
extern void
vk_app_state_destroy_presentation(vk_app_state_t * app_state);

//
//
//

#ifdef __cplusplus
}
#endif

//
//
//

#endif  // SRC_GRAPHICS_LIB_COMPUTE_COMMON_VK_VK_APP_STATE_H_
