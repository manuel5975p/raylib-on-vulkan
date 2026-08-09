// raylib-on-vulkan example: image generation + texture drawing
#include "raylib.h"

#include <stdlib.h>

int main(void)
{
    const char *framesEnv = getenv("RAYVK_FRAMES");
    int maxFrames = (framesEnv != NULL)? atoi(framesEnv) : 0;

    InitWindow(800, 450, "rayvulkan - textures");
    SetTargetFPS(60);

    Image checked = GenImageChecked(128, 128, 16, 16, RED, GOLD);
    Image gradient = GenImageGradientLinear(128, 128, 0, DARKBLUE, SKYBLUE);
    Image noise = GenImagePerlinNoise(128, 128, 0, 0, 4.0f);
    ImageBlurGaussian(&noise, 2);

    Texture2D texChecked = LoadTextureFromImage(checked);
    Texture2D texGradient = LoadTextureFromImage(gradient);
    Texture2D texNoise = LoadTextureFromImage(noise);
    UnloadImage(checked);
    UnloadImage(gradient);
    UnloadImage(noise);

    float rotation = 0.0f;
    int frames = 0;
    while (!WindowShouldClose())
    {
        rotation += 0.5f;

        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawTexture(texChecked, 60, 100, WHITE);
            DrawTextureEx(texGradient, (Vector2){ 260, 100 }, 0.0f, 1.0f, WHITE);
            DrawTexturePro(texNoise,
                (Rectangle){ 0, 0, 128, 128 },
                (Rectangle){ 550, 164, 128, 128 },
                (Vector2){ 64, 64 }, rotation, WHITE);
            DrawText("checked", 60, 240, 20, DARKGRAY);
            DrawText("gradient", 260, 240, 20, DARKGRAY);
            DrawText("perlin (rotating)", 480, 240, 20, DARKGRAY);
        EndDrawing();

        frames++;
        if ((maxFrames > 0) && (frames >= maxFrames)) break;
    }

    UnloadTexture(texChecked);
    UnloadTexture(texGradient);
    UnloadTexture(texNoise);
    CloseWindow();
    return 0;
}
