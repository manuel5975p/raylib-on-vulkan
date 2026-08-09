/**********************************************************************************************
*
*   rayvk - Public Vulkan interop API for raylib-on-vulkan
*
*   Lets applications record and submit their own Vulkan commands alongside the
*   raylib drawing API. Include <vulkan/vulkan.h> (or volk.h) BEFORE this header,
*   or the handle-typed accessors stay hidden and only the void* / uint64 variants
*   are declared.
*
*   Two usage patterns:
*
*   1) Inject into the current frame (between BeginDrawing/EndDrawing):
*        VkCommandBuffer cmd = GetVkCurrentCommandBuffer();
*        // NOTE: a render pass targeting the swapchain image may be active;
*        // call BeginVkCustomMode()/EndVkCustomMode() to get the command buffer
*        // with the render pass suspended (batch flushed, render pass ended).
*
*   2) One-shot submissions outside the frame (uploads, compute, blits):
*        VkCommandBuffer cmd = BeginVkCommands();
*        ...record...
*        EndVkCommands(cmd);          // submits on graphics queue and waits
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#ifndef RAYVK_H
#define RAYVK_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

// Raw accessors (always available; cast to the corresponding Vk* handle)
void *GetVkInstance(void);              // VkInstance
void *GetVkPhysicalDevice(void);        // VkPhysicalDevice
void *GetVkDevice(void);                // VkDevice
void *GetVkGraphicsQueue(void);         // VkQueue
uint32_t GetVkGraphicsQueueFamily(void);
uint64_t GetVkSwapchain(void);          // VkSwapchainKHR
uint64_t GetVkSwapchainRenderPass(void);// VkRenderPass compatible with default target
int GetVkSwapchainImageCount(void);
uint32_t GetVkSwapchainImageFormat(void);   // VkFormat
uint64_t GetVkSwapchainImage(int index);    // VkImage
uint32_t GetVkFrameIndex(void);         // Current frame-in-flight index

// Frame injection: valid only between BeginDrawing() and EndDrawing()
void *GetVkCurrentCommandBuffer(void);  // VkCommandBuffer of the frame being recorded

// Suspend raylib-on-vulkan's render pass so custom commands (dispatches, barriers,
// copies, own render passes) can be recorded into the frame command buffer.
// Returns the frame VkCommandBuffer. Must be paired with EndVkCustomMode(),
// which restores raylib-on-vulkan's render pass and pipeline state.
void *BeginVkCustomMode(void);
void EndVkCustomMode(void);

// One-shot commands: allocate/begin a primary command buffer on the graphics
// queue family. EndVkCommands() ends, submits, waits for completion and frees it.
// SubmitVkCommands() is the non-blocking variant; the buffer is freed once the
// internally-signaled fence is observed on a later frame boundary.
void *BeginVkCommands(void);            // VkCommandBuffer
void EndVkCommands(void *commandBuffer);
void SubmitVkCommands(void *commandBuffer, uint64_t waitSemaphore, uint64_t signalSemaphore);

#if defined(__cplusplus)
}
#endif

#endif // RAYVK_H
