// raylib-on-vulkan example: custom Vulkan command recording via rayvk.h
//
// Demonstrates the interop API: a raw vkCmdClearAttachments call is recorded
// into the frame command buffer between raylib draws, and device info is
// queried directly from Vulkan.
#include "volk.h"      // brings vulkan types; VK_NO_PROTOTYPES is set by the build
#include "raylib.h"
#include "rayvk.h"

#include <stdlib.h>

int main(void)
{
    const char *framesEnv = getenv("RAYVK_FRAMES");
    int maxFrames = (framesEnv != NULL)? atoi(framesEnv) : 0;

    InitWindow(800, 450, "rayvulkan - custom Vulkan commands");
    SetTargetFPS(60);

    VkPhysicalDevice gpu = (VkPhysicalDevice)GetVkPhysicalDevice();
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);

    int frames = 0;
    while (!WindowShouldClose())
    {
        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawText("below: rectangle cleared by raw vkCmdClearAttachments", 40, 60, 20, DARKGRAY);

            // Custom mode suspends raylib-on-vulkan's render pass and hands over the
            // frame command buffer; here we immediately re-enter our own usage
            // of the *active* pass instead: simplest interop is recording into
            // the current pass-compatible command buffer.
            VkCommandBuffer cmd = (VkCommandBuffer)GetVkCurrentCommandBuffer();
            if (cmd != VK_NULL_HANDLE)
            {
                VkClearAttachment att = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .colorAttachment = 0,
                    .clearValue = { .color = { .float32 = { 0.2f, 0.4f, 0.9f, 1.0f } } },
                };
                VkClearRect rect = {
                    .rect = { .offset = { 40, 120 }, .extent = { 300, 180 } },
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                };
                // Force pending batched draws out first so ordering is correct
                // (DrawText above must land before our clear).
                // The batch flush happens inside GetVkCurrentCommandBuffer-safe
                // interop points; BeginVkCustomMode()/EndVkCustomMode() would do
                // it for heavyweight usage. For an in-pass command this is fine:
                vkCmdClearAttachments(cmd, 1, &att, 1, &rect);
            }

            DrawText(TextFormat("GPU: %s", props.deviceName), 40, 330, 20, DARKBLUE);
            DrawText(TextFormat("Vulkan API: %d.%d.%d",
                     VK_API_VERSION_MAJOR(props.apiVersion),
                     VK_API_VERSION_MINOR(props.apiVersion),
                     VK_API_VERSION_PATCH(props.apiVersion)), 40, 360, 20, DARKBLUE);
            DrawFPS(700, 10);
        EndDrawing();

        frames++;
        if ((maxFrames > 0) && (frames >= maxFrames)) break;
    }

    CloseWindow();
    return 0;
}
