// Tests: window lifecycle + drawing smoke test (needs display + Vulkan device)
#include "raylib.h"
#include "testkit.h"

int main(void)
{
    SetTraceLogLevel(LOG_DEBUG);

    InitWindow(320, 240, "rayvk smoke");
    CHECK(IsWindowReady());
    CHECK_EQ_INT(GetScreenWidth(), 320);
    CHECK_EQ_INT(GetScreenHeight(), 240);
    CHECK(GetRenderWidth() >= 320);
    CHECK(GetMonitorCount() >= 1);
    CHECK(GetWindowHandle() != NULL);

    SetTargetFPS(60);
    double t0 = GetTime();
    for (int i = 0; i < 10; i++)
    {
        BeginDrawing();
            ClearBackground(DARKBLUE);
            DrawRectangle(10, 10, 100, 50, RED);
            DrawCircle(200, 120, 40, GREEN);
            DrawText("smoke", 10, 200, 20, WHITE);
        EndDrawing();
    }
    CHECK(GetTime() > t0);
    CHECK(GetFrameTime() >= 0.0f);

    // Texture upload/draw inside the loop context
    Image img = GenImageChecked(32, 32, 8, 8, RED, BLUE);
    Texture2D tex = LoadTextureFromImage(img);
    CHECK(IsTextureValid(tex));
    CHECK_EQ_INT(tex.width, 32);
    UnloadImage(img);

    RenderTexture2D rt = LoadRenderTexture(64, 64);
    CHECK(IsRenderTextureValid(rt));

    BeginDrawing();
        BeginTextureMode(rt);
            ClearBackground(GREEN);
            DrawRectangle(0, 0, 16, 16, RED);
        EndTextureMode();
        ClearBackground(BLACK);
        DrawTexture(rt.texture, 0, 0, WHITE);
        DrawTexture(tex, 100, 100, WHITE);
    EndDrawing();

    // Read back render texture: (0,0) painted RED (top-left after flip handling)
    Image rtImg = LoadImageFromTexture(rt.texture);
    CHECK(IsImageValid(rtImg));
    Color pixelCorner = GetImageColor(rtImg, 2, 2);
    Color pixelMid = GetImageColor(rtImg, 40, 40);
    // One of the corners must be red, body green (exact origin depends on flip convention)
    bool cornerRed = (pixelCorner.r > 200) && (pixelCorner.g < 100);
    Color pixelOther = GetImageColor(rtImg, 2, 61);
    bool otherRed = (pixelOther.r > 200) && (pixelOther.g < 100);
    CHECK(cornerRed || otherRed);
    CHECK((pixelMid.g > 200) && (pixelMid.r < 100));
    UnloadImage(rtImg);

    UnloadRenderTexture(rt);
    UnloadTexture(tex);

    CHECK(!WindowShouldClose());
    CloseWindow();
    CHECK(!IsWindowReady());

    return tk_report("test_window_smoke");
}
