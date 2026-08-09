// raylib-on-vulkan example: text drawing and measuring
#include "raylib.h"

#include <stdlib.h>

int main(void)
{
    const char *framesEnv = getenv("RAYVK_FRAMES");
    int maxFrames = (framesEnv != NULL)? atoi(framesEnv) : 0;

    InitWindow(800, 450, "rayvulkan - text");
    SetTargetFPS(60);

    const char *msg = "rayvulkan: raylib on pure Vulkan";
    int frames = 0;
    while (!WindowShouldClose())
    {
        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawText(msg, 40, 80, 20, DARKGRAY);
            DrawText(TextFormat("MeasureText: %d px", MeasureText(msg, 20)), 40, 120, 20, MAROON);
            DrawText(TextFormat("Frame: %u  Time: %.2f", frames, GetTime()), 40, 160, 20, DARKBLUE);
            DrawText("BIG TEXT", 40, 220, 60, BLACK);
            DrawFPS(700, 10);
        EndDrawing();

        frames++;
        if ((maxFrames > 0) && (frames >= maxFrames)) break;
    }

    CloseWindow();
    return 0;
}
