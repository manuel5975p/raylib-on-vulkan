// Tests: CPU image generation/manipulation (no window needed)
#include "raylib.h"
#include "testkit.h"

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);

    // Generation
    Image solid = GenImageColor(8, 8, RED);
    CHECK(IsImageValid(solid));
    CHECK_EQ_INT(solid.width, 8);
    CHECK(ColorIsEqual(GetImageColor(solid, 3, 3), RED));

    Image checked = GenImageChecked(8, 8, 4, 4, BLACK, WHITE);
    CHECK(ColorIsEqual(GetImageColor(checked, 0, 0), BLACK));
    CHECK(ColorIsEqual(GetImageColor(checked, 4, 0), WHITE));

    // Copy / crop / sub-image
    Image copy = ImageCopy(solid);
    CHECK(IsImageValid(copy));
    CHECK(copy.data != solid.data);
    ImageCrop(&copy, (Rectangle){ 2, 2, 4, 4 });
    CHECK_EQ_INT(copy.width, 4);
    CHECK_EQ_INT(copy.height, 4);

    Image piece = ImageFromImage(checked, (Rectangle){ 0, 0, 4, 4 });
    CHECK_EQ_INT(piece.width, 4);
    CHECK(ColorIsEqual(GetImageColor(piece, 0, 0), BLACK));

    // Format conversion roundtrip RGBA8 -> R5G6B5 -> RGBA8
    Image conv = GenImageColor(4, 4, (Color){ 255, 0, 0, 255 });
    ImageFormat(&conv, PIXELFORMAT_UNCOMPRESSED_R5G6B5);
    CHECK_EQ_INT(conv.format, PIXELFORMAT_UNCOMPRESSED_R5G6B5);
    ImageFormat(&conv, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color rc = GetImageColor(conv, 0, 0);
    CHECK(rc.r > 240);
    CHECK_EQ_INT(rc.g, 0);

    // Flips and rotations
    Image grad = GenImageGradientLinear(4, 4, 0, BLACK, WHITE);   // vertical
    Color topBefore = GetImageColor(grad, 0, 0);
    ImageFlipVertical(&grad);
    Color topAfter = GetImageColor(grad, 0, 0);
    CHECK(topBefore.r != topAfter.r);

    Image rot = GenImageColor(2, 4, BLUE);
    ImageRotateCW(&rot);
    CHECK_EQ_INT(rot.width, 4);
    CHECK_EQ_INT(rot.height, 2);

    // Resize
    Image rs = GenImageColor(4, 4, GREEN);
    ImageResize(&rs, 8, 8);
    CHECK_EQ_INT(rs.width, 8);
    ImageResizeNN(&rs, 2, 2);
    CHECK_EQ_INT(rs.width, 2);
    CHECK(ColorIsEqual(GetImageColor(rs, 0, 0), GREEN));

    // Canvas resize with fill
    Image cv = GenImageColor(2, 2, RED);
    ImageResizeCanvas(&cv, 4, 4, 1, 1, BLUE);
    CHECK(ColorIsEqual(GetImageColor(cv, 0, 0), BLUE));
    CHECK(ColorIsEqual(GetImageColor(cv, 1, 1), RED));

    // Color ops
    Image inv = GenImageColor(2, 2, (Color){ 10, 20, 30, 255 });
    ImageColorInvert(&inv);
    CHECK_EQ_INT(GetImageColor(inv, 0, 0).r, 245);

    Image gray = GenImageColor(2, 2, (Color){ 255, 0, 0, 255 });
    ImageColorGrayscale(&gray);
    Color g = GetImageColor(gray, 0, 0);
    CHECK_EQ_INT(g.r, g.g);
    CHECK_EQ_INT(g.g, g.b);

    Image repl = GenImageColor(2, 2, RED);
    ImageColorReplace(&repl, RED, GOLD);
    CHECK(ColorIsEqual(GetImageColor(repl, 0, 0), GOLD));

    // Drawing into image
    Image canvas = GenImageColor(16, 16, BLACK);
    ImageDrawPixel(&canvas, 8, 8, WHITE);
    CHECK(ColorIsEqual(GetImageColor(canvas, 8, 8), WHITE));
    ImageDrawRectangle(&canvas, 0, 0, 4, 4, RED);
    CHECK(ColorIsEqual(GetImageColor(canvas, 2, 2), RED));
    ImageDrawCircle(&canvas, 12, 12, 2, BLUE);
    CHECK(ColorIsEqual(GetImageColor(canvas, 12, 12), BLUE));
    ImageDrawLine(&canvas, 0, 15, 15, 15, GREEN);
    CHECK(ColorIsEqual(GetImageColor(canvas, 7, 15), GREEN));

    // Alpha ops
    Image am = GenImageColor(2, 2, (Color){ 100, 100, 100, 128 });
    ImageAlphaClear(&am, MAGENTA, 0.9f);
    CHECK(ColorIsEqual(GetImageColor(am, 0, 0), MAGENTA));

    Image pm = GenImageColor(1, 1, (Color){ 200, 100, 50, 128 });
    ImageAlphaPremultiply(&pm);
    Color pmc = GetImageColor(pm, 0, 0);
    CHECK(pmc.r <= 101 && pmc.r >= 99);   // 200*128/255 ≈ 100

    // Palette / colors
    Image pal = GenImageChecked(4, 4, 2, 2, RED, BLUE);
    int colorCount = 0;
    Color *colors = LoadImagePalette(pal, 16, &colorCount);
    CHECK_EQ_INT(colorCount, 2);
    UnloadImagePalette(colors);
    Color *all = LoadImageColors(pal);
    CHECK(all != NULL);
    UnloadImageColors(all);

    // Alpha border
    Image ab = GenImageColor(8, 8, BLANK);
    ImageDrawRectangle(&ab, 2, 2, 3, 3, RED);
    Rectangle border = GetImageAlphaBorder(ab, 0.5f);
    CHECK_EQ_INT((int)border.x, 2);
    CHECK_EQ_INT((int)border.width, 3);

    // Export/import roundtrip via memory (PNG)
    Image out = GenImageChecked(8, 8, 2, 2, RED, GREEN);
    int fileSize = 0;
    unsigned char *png = ExportImageToMemory(out, ".png", &fileSize);
    CHECK(png != NULL);
    CHECK(fileSize > 0);
    Image in = LoadImageFromMemory(".png", png, fileSize);
    CHECK(IsImageValid(in));
    CHECK_EQ_INT(in.width, 8);
    CHECK(ColorIsEqual(GetImageColor(in, 0, 0), RED));
    MemFree(png);

    // Mipmaps
    Image mip = GenImageColor(16, 16, RED);
    ImageMipmaps(&mip);
    CHECK(mip.mipmaps > 1);

    UnloadImage(solid); UnloadImage(checked); UnloadImage(copy); UnloadImage(piece);
    UnloadImage(conv); UnloadImage(grad); UnloadImage(rot); UnloadImage(rs);
    UnloadImage(cv); UnloadImage(inv); UnloadImage(gray); UnloadImage(repl);
    UnloadImage(canvas); UnloadImage(am); UnloadImage(pm); UnloadImage(pal);
    UnloadImage(ab); UnloadImage(out); UnloadImage(in); UnloadImage(mip);

    return tk_report("test_images");
}
