// raylib-on-vulkan example: 3D geometric shapes with camera
#include "raylib.h"

#include <stdlib.h>

int main(void)
{
    const char *framesEnv = getenv("RAYVK_FRAMES");
    int maxFrames = (framesEnv != NULL)? atoi(framesEnv) : 0;

    InitWindow(800, 450, "rayvulkan - 3D models");
    SetTargetFPS(60);

    Camera3D camera = {
        .position = { 10.0f, 10.0f, 10.0f },
        .target = { 0.0f, 0.0f, 0.0f },
        .up = { 0.0f, 1.0f, 0.0f },
        .fovy = 45.0f,
        .projection = CAMERA_PERSPECTIVE,
    };

    int frames = 0;
    while (!WindowShouldClose())
    {
        UpdateCamera(&camera, CAMERA_ORBITAL);

        BeginDrawing();
            ClearBackground(RAYWHITE);
            BeginMode3D(camera);
                DrawCube((Vector3){ -4, 0.5f, 0 }, 1, 1, 1, RED);
                DrawCubeWires((Vector3){ -4, 0.5f, 0 }, 1, 1, 1, MAROON);
                DrawSphere((Vector3){ -1, 1, 0 }, 1.0f, GREEN);
                DrawSphereWires((Vector3){ -1, 1, 0 }, 1.05f, 8, 8, DARKGREEN);
                DrawCylinder((Vector3){ 2, 0, 0 }, 0.5f, 0.8f, 2.0f, 12, SKYBLUE);
                DrawCapsule((Vector3){ 5, 0.5f, 0 }, (Vector3){ 5, 2.0f, 0 }, 0.5f, 8, 8, VIOLET);
                DrawGrid(10, 1.0f);
            EndMode3D();
            DrawFPS(10, 10);
        EndDrawing();

        frames++;
        if ((maxFrames > 0) && (frames >= maxFrames)) break;
    }

    CloseWindow();
    return 0;
}
