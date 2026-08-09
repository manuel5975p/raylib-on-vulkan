// Tests: pixel-exact rendering verification via screen readback (needs display + Vulkan)
#include "raylib.h"
#include "testkit.h"

static bool ColorsClose(Color a, Color b, int tol)
{
    int dr = (int)a.r - (int)b.r;
    int dg = (int)a.g - (int)b.g;
    int db = (int)a.b - (int)b.b;
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return (dr <= tol) && (dg <= tol) && (db <= tol);
}

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);

    InitWindow(256, 256, "rayvk readback");

    // Draw a deterministic frame: red top-left quadrant, green bottom-right,
    // blue background elsewhere.
    BeginDrawing();
        ClearBackground(BLUE);
        DrawRectangle(0, 0, 128, 128, RED);
        DrawRectangle(128, 128, 128, 128, GREEN);
    EndDrawing();

    // Draw the same frame again and read it back before present of a third one:
    // LoadImageFromScreen captures the last completed frame.
    BeginDrawing();
        ClearBackground(BLUE);
        DrawRectangle(0, 0, 128, 128, RED);
        DrawRectangle(128, 128, 128, 128, GREEN);
    EndDrawing();

    Image screen = LoadImageFromScreen();
    CHECK(IsImageValid(screen));
    CHECK_EQ_INT(screen.width, GetRenderWidth());

    if (IsImageValid(screen) && (screen.width >= 256) && (screen.height >= 256))
    {
        Color tl = GetImageColor(screen, 64, 64);       // expect RED
        Color br = GetImageColor(screen, 192, 192);     // expect GREEN
        Color tr = GetImageColor(screen, 192, 64);      // expect BLUE
        Color bl = GetImageColor(screen, 64, 192);      // expect BLUE
        CHECK(ColorsClose(tl, RED, 12));
        CHECK(ColorsClose(br, GREEN, 12));
        CHECK(ColorsClose(tr, BLUE, 12));
        CHECK(ColorsClose(bl, BLUE, 12));
    }
    UnloadImage(screen);

    // Scissor: clear full blue, then scissored red fill must only affect region
    BeginDrawing();
        ClearBackground(BLUE);
        BeginScissorMode(0, 0, 64, 64);
            DrawRectangle(0, 0, 256, 256, RED);
        EndScissorMode();
    EndDrawing();
    BeginDrawing();
        ClearBackground(BLUE);
        BeginScissorMode(0, 0, 64, 64);
            DrawRectangle(0, 0, 256, 256, RED);
        EndScissorMode();
    EndDrawing();

    Image sc = LoadImageFromScreen();
    if (IsImageValid(sc) && (sc.width >= 256))
    {
        CHECK(ColorsClose(GetImageColor(sc, 32, 32), RED, 12));
        CHECK(ColorsClose(GetImageColor(sc, 128, 128), BLUE, 12));
    }
    UnloadImage(sc);

    CloseWindow();
    return tk_report("test_render_readback");
}
