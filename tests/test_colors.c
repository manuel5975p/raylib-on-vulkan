// Tests: color conversion/manipulation functions (no window needed)
#include "raylib.h"
#include "testkit.h"

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);

    CHECK(ColorIsEqual(RED, (Color){ 230, 41, 55, 255 }));
    CHECK(!ColorIsEqual(RED, BLUE));

    CHECK_EQ_INT((unsigned int)ColorToInt((Color){ 0x11, 0x22, 0x33, 0x44 }), 0x11223344u);
    Color c = GetColor(0xAABBCCDDu);
    CHECK_EQ_INT(c.r, 0xAA); CHECK_EQ_INT(c.g, 0xBB); CHECK_EQ_INT(c.b, 0xCC); CHECK_EQ_INT(c.a, 0xDD);

    Vector4 n = ColorNormalize((Color){ 255, 0, 128, 255 });
    CHECK_NEAR(n.x, 1.0, 1e-6); CHECK_NEAR(n.z, 128.0/255.0, 1e-6);
    Color cn = ColorFromNormalized((Vector4){ 1.0f, 0.0f, 0.5f, 1.0f });
    CHECK_EQ_INT(cn.r, 255); CHECK_EQ_INT(cn.b, 127);

    // HSV roundtrip on pure red
    Vector3 hsv = ColorToHSV((Color){ 255, 0, 0, 255 });
    CHECK_NEAR(hsv.x, 0.0, 0.5); CHECK_NEAR(hsv.y, 1.0, 1e-3); CHECK_NEAR(hsv.z, 1.0, 1e-3);
    Color red2 = ColorFromHSV(0.0f, 1.0f, 1.0f);
    CHECK_EQ_INT(red2.r, 255); CHECK_EQ_INT(red2.g, 0); CHECK_EQ_INT(red2.b, 0);

    Color faded = Fade((Color){ 10, 20, 30, 255 }, 0.5f);
    CHECK_EQ_INT(faded.a, 127);
    Color alpha = ColorAlpha((Color){ 10, 20, 30, 255 }, 0.0f);
    CHECK_EQ_INT(alpha.a, 0);

    Color tinted = ColorTint(WHITE, (Color){ 128, 128, 128, 255 });
    CHECK_EQ_INT(tinted.r, 128);

    Color lerped = ColorLerp(BLACK, WHITE, 0.5f);
    CHECK(lerped.r >= 127 && lerped.r <= 128);

    // Brightness/contrast bounds
    Color bright = ColorBrightness(BLACK, 1.0f);
    CHECK_EQ_INT(bright.r, 255);
    Color dark = ColorBrightness(WHITE, -1.0f);
    CHECK_EQ_INT(dark.r, 0);

    // Pixel data size + set/get roundtrip
    CHECK_EQ_INT(GetPixelDataSize(4, 4, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8), 64);
    CHECK_EQ_INT(GetPixelDataSize(4, 4, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE), 16);
    CHECK_EQ_INT(GetPixelDataSize(2, 2, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32), 64);

    unsigned char px[4] = { 0 };
    SetPixelColor(px, (Color){ 1, 2, 3, 4 }, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color got = GetPixelColor(px, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    CHECK_EQ_INT(got.r, 1); CHECK_EQ_INT(got.a, 4);

    unsigned short px16 = 0;
    SetPixelColor(&px16, (Color){ 255, 0, 0, 255 }, PIXELFORMAT_UNCOMPRESSED_R5G6B5);
    Color got16 = GetPixelColor(&px16, PIXELFORMAT_UNCOMPRESSED_R5G6B5);
    CHECK(got16.r > 240); CHECK_EQ_INT(got16.g, 0);

    return tk_report("test_colors");
}
