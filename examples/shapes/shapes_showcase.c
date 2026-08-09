// raylib-on-vulkan example: 2D shapes showcase
#include "raylib.h"

#include <stdlib.h>

int main(void)
{
    const char *framesEnv = getenv("RAYVK_FRAMES");
    int maxFrames = (framesEnv != NULL)? atoi(framesEnv) : 0;

    InitWindow(800, 450, "rayvulkan - shapes");
    SetTargetFPS(60);

    float rotation = 0.0f;
    int frames = 0;
    while (!WindowShouldClose())
    {
        rotation += 0.4f;

        BeginDrawing();
            ClearBackground(RAYWHITE);

            DrawCircle(100, 120, 35, DARKBLUE);
            DrawCircleGradient((Vector2){ 100, 220 }, 30, GREEN, SKYBLUE);
            DrawCircleLines(100, 340, 40, DARKBLUE);

            DrawRectangle(230, 100, 60, 60, RED);
            DrawRectangleLines(230, 200, 60, 80, GOLD);
            DrawRectangleGradientH(230, 320, 60, 60, MAROON, GOLD);
            DrawRectangleRounded((Rectangle){ 320, 100, 80, 60 }, 0.4f, 8, LIME);

            DrawTriangle((Vector2){ 480, 100 }, (Vector2){ 440, 180 }, (Vector2){ 520, 180 }, VIOLET);
            DrawPoly((Vector2){ 480, 280 }, 6, 50, rotation, BROWN);
            DrawPolyLinesEx((Vector2){ 480, 280 }, 6, 60, rotation, 4, BEIGE);

            DrawLine(580, 100, 750, 100, BLACK);
            DrawLineEx((Vector2){ 580, 140 }, (Vector2){ 750, 160 }, 5.0f, PURPLE);
            DrawLineBezier((Vector2){ 580, 200 }, (Vector2){ 750, 260 }, 3.0f, ORANGE);
            DrawLineDashed((Vector2){ 580, 300 }, (Vector2){ 750, 300 }, 8, 6, DARKGRAY);

            DrawRing((Vector2){ 660, 380 }, 25, 40, 0, 270, 24, SKYBLUE);
            DrawEllipse(300, 400, 50, 25, PINK);
        EndDrawing();

        frames++;
        if ((maxFrames > 0) && (frames >= maxFrames)) break;
    }

    CloseWindow();
    return 0;
}
