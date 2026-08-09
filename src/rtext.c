/**********************************************************************************************
*
*   rtext - Font loading, text drawing, text measuring, codepoints and string utilities
*
*   raylib-on-vulkan reimplementation of raylib's rtext module. Rendering goes exclusively through
*   the rlvk immediate-mode layer (rlBegin(RL_QUADS)/rlSetTexture), never through
*   DrawTexture*() helpers, so text batches together with shapes/textures in one draw call.
*
*   MODULE OWNERSHIP (see CONVENTIONS.md)
*     - STB_TRUETYPE_IMPLEMENTATION and STB_RECT_PACK_IMPLEMENTATION are defined here.
*
*   INTERNAL CONTRACT ADDITION (documented as required by CONVENTIONS.md)
*     - LoadFontDefault(void) / UnloadFontDefault(void) are exported (non-static) so rcore
*       can build/release the embedded default font at InitWindow()/CloseWindow() time,
*       exactly like raylib does. They are idempotent: GetFontDefault() also builds the
*       font lazily, so rcore is free to ignore them entirely.
*
*   DEFAULT FONT
*     The default font is a bitmap font embedded in this file (original artwork, no
*     third-party data). Glyphs cover ASCII 32..126 (95 glyphs), which includes the
*     0x3f ('?') GLYPH_NOTFOUND_CHAR_FALLBACK. Metrics, all in font pixels:
*       - Source cell:   6 columns x 8 rows, 1 bit per pixel, MSB = leftmost column.
*                        Drawing area is 5 columns wide, column 5 is inter-glyph space.
*       - Baseline:      bottom edge of row 6. Rows 0..6 are above the baseline
*                        (cap height 7 px, x-height 5 px), row 7 holds descenders.
*       - baseSize:      10, matching raylib's default font base size. DrawText()/
*                        MeasureText() derive spacing as fontSize/10, so any font size
*                        that is a multiple of 10 maps to an integer scale factor and
*                        renders pixel-exact with the default point (nearest) filter.
*       - offsetY:       1 for every glyph => the 8 px cell sits at rows 1..8 of the
*                        10 px line box, leaving 1 px of leading above and below.
*       - offsetX:       0, advanceX: 6 (monospaced advance, 5 px ink + 1 px gap).
*       - glyphPadding:  1 (atlas padding, kept transparent by GenImageFontAtlas()).
*     The atlas is built through the very same GenImageFontAtlas() exposed publicly, so
*     the default font is byte-for-byte a regular font as far as the rest of the API is
*     concerned (GRAY_ALPHA 128x128 atlas).
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#include "raylib.h"

#if !defined(EXTERNAL_CONFIG_FLAGS)
    #include "config.h"         // SUPPORT_MODULE_RTEXT, SUPPORT_FILEFORMAT_TTF/FNT/BDF
#endif

#include "rlvk.h"               // rlBegin/rlEnd/rlSetTexture/... immediate-mode layer

#include <stdlib.h>             // Required for: malloc(), free()
#include <stdio.h>              // Required for: vsnprintf(), sprintf()
#include <string.h>             // Required for: strcmp(), strstr(), memcpy(), memset()
#include <stdarg.h>             // Required for: va_list, va_start(), va_end()

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
#ifndef RL_MALLOC
    #define RL_MALLOC(sz)       malloc(sz)
#endif
#ifndef RL_CALLOC
    #define RL_CALLOC(n,sz)     calloc(n,sz)
#endif
#ifndef RL_REALLOC
    #define RL_REALLOC(ptr,sz)  realloc(ptr,sz)
#endif
#ifndef RL_FREE
    #define RL_FREE(ptr)        free(ptr)
#endif

#ifndef TRACELOG
    #define TRACELOG(level, ...) TraceLog(level, __VA_ARGS__)
#endif

// Text buffers, replicating raylib static-buffer semantics
#ifndef MAX_TEXT_BUFFER_LENGTH
    #define MAX_TEXT_BUFFER_LENGTH      1024    // Size of internal static buffers used on some functions
#endif
#ifndef MAX_TEXTSPLIT_COUNT
    #define MAX_TEXTSPLIT_COUNT          128    // Maximum number of substrings to split: TextSplit()
#endif
#ifndef MAX_TEXTFORMAT_BUFFERS
    #define MAX_TEXTFORMAT_BUFFERS         4    // Maximum number of static buffers for text formatting
#endif

// Default font/TTF generation parameters (same values as raylib)
#ifndef FONT_TTF_DEFAULT_SIZE
    #define FONT_TTF_DEFAULT_SIZE         32    // TTF font generation default char size (char-height)
#endif
#ifndef FONT_TTF_DEFAULT_NUMCHARS
    #define FONT_TTF_DEFAULT_NUMCHARS     95    // TTF font generation default charset: 95 glyphs (ASCII 32..126)
#endif
#ifndef FONT_TTF_DEFAULT_FIRST_CHAR
    #define FONT_TTF_DEFAULT_FIRST_CHAR   32    // TTF font generation default first char for image sprite font (32-Space)
#endif
#ifndef FONT_TTF_DEFAULT_CHARS_PADDING
    #define FONT_TTF_DEFAULT_CHARS_PADDING 4    // TTF font generation default chars padding
#endif

#define FONT_BITMAP_ALPHA_THRESHOLD       80    // Bitmap (B&W) font generation alpha threshold
#define FONT_SDF_CHAR_PADDING              4    // SDF font generation char padding
#define FONT_SDF_ON_EDGE_VALUE           128    // SDF font generation on edge value
#define FONT_SDF_PIXEL_DIST_SCALE      64.0f    // SDF font generation pixel distance scale

#define GLYPH_NOTFOUND_CHAR_FALLBACK      63    // Character used if requested codepoint is not found: '?'

// Embedded default font geometry (see module header for the rationale)
#define FONT_DEFAULT_FIRST_CHAR           32
#define FONT_DEFAULT_GLYPH_COUNT          95
#define FONT_DEFAULT_CELL_WIDTH            6
#define FONT_DEFAULT_CELL_HEIGHT           8
#define FONT_DEFAULT_BASE_SIZE            10
#define FONT_DEFAULT_GLYPH_OFFSET_Y        1
#define FONT_DEFAULT_ADVANCE_X             6
#define FONT_DEFAULT_GLYPH_PADDING         1

#define TEXT_COLOR_EQUAL(c1, c2) (((c1).r == (c2).r) && ((c1).g == (c2).g) && ((c1).b == (c2).b) && ((c1).a == (c2).a))

//----------------------------------------------------------------------------------
// Third-party implementations owned by this module
//----------------------------------------------------------------------------------
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"      // Required by: GenImageFontAtlas() skyline packing

#if defined(SUPPORT_FILEFORMAT_TTF) && SUPPORT_FILEFORMAT_TTF
    #define STBTT_malloc(x,u)   ((void)(u), RL_MALLOC(x))
    #define STBTT_free(x,u)     ((void)(u), RL_FREE(x))
    #define STB_TRUETYPE_IMPLEMENTATION
    #include "stb_truetype.h"   // Required by: LoadFontData()
#endif

//----------------------------------------------------------------------------------
// Module internal data
//----------------------------------------------------------------------------------

// Embedded default font: 95 glyphs (ASCII 32..126), 8 rows per glyph,
// bit 7 (0x80) of every row byte is the leftmost pixel of a 6 px wide cell
static const unsigned char fontDefaultData[FONT_DEFAULT_GLYPH_COUNT*FONT_DEFAULT_CELL_HEIGHT] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00,
    0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0xf8, 0x50, 0xf8, 0x50, 0x00, 0x00,
    0x20, 0x78, 0xa0, 0x70, 0x28, 0xf0, 0x20, 0x00, 0xc0, 0xc8, 0x10, 0x20, 0x40, 0x98, 0x18, 0x00,
    0x60, 0x90, 0xa0, 0x40, 0xa8, 0x90, 0x68, 0x00, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x20, 0x40, 0x40, 0x40, 0x20, 0x10, 0x00, 0x40, 0x20, 0x10, 0x10, 0x10, 0x20, 0x40, 0x00,
    0x00, 0x20, 0xa8, 0x70, 0xa8, 0x20, 0x00, 0x00, 0x00, 0x20, 0x20, 0xf8, 0x20, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x08, 0x10, 0x10, 0x20, 0x40, 0x40, 0x80, 0x00,
    0x70, 0x88, 0x98, 0xa8, 0xc8, 0x88, 0x70, 0x00, 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00,
    0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xf8, 0x00, 0xf8, 0x10, 0x20, 0x10, 0x08, 0x88, 0x70, 0x00,
    0x10, 0x30, 0x50, 0x90, 0xf8, 0x10, 0x10, 0x00, 0xf8, 0x80, 0xf0, 0x08, 0x08, 0x88, 0x70, 0x00,
    0x30, 0x40, 0x80, 0xf0, 0x88, 0x88, 0x70, 0x00, 0xf8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40, 0x00,
    0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00, 0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60, 0x00,
    0x00, 0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x20, 0x40, 0x00,
    0x00, 0x10, 0x20, 0x40, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x00, 0xf8, 0x00, 0x00, 0x00,
    0x00, 0x40, 0x20, 0x10, 0x20, 0x40, 0x00, 0x00, 0x70, 0x88, 0x08, 0x10, 0x20, 0x00, 0x20, 0x00,
    0x70, 0x88, 0xb8, 0xa8, 0xb8, 0x80, 0x70, 0x00, 0x70, 0x88, 0x88, 0xf8, 0x88, 0x88, 0x88, 0x00,
    0xf0, 0x88, 0x88, 0xf0, 0x88, 0x88, 0xf0, 0x00, 0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70, 0x00,
    0xf0, 0x88, 0x88, 0x88, 0x88, 0x88, 0xf0, 0x00, 0xf8, 0x80, 0x80, 0xf0, 0x80, 0x80, 0xf8, 0x00,
    0xf8, 0x80, 0x80, 0xf0, 0x80, 0x80, 0x80, 0x00, 0x70, 0x88, 0x80, 0xb8, 0x88, 0x88, 0x70, 0x00,
    0x88, 0x88, 0x88, 0xf8, 0x88, 0x88, 0x88, 0x00, 0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00,
    0x38, 0x10, 0x10, 0x10, 0x10, 0x90, 0x60, 0x00, 0x88, 0x90, 0xa0, 0xc0, 0xa0, 0x90, 0x88, 0x00,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xf8, 0x00, 0x88, 0xd8, 0xa8, 0xa8, 0x88, 0x88, 0x88, 0x00,
    0x88, 0x88, 0xc8, 0xa8, 0x98, 0x88, 0x88, 0x00, 0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00,
    0xf0, 0x88, 0x88, 0xf0, 0x80, 0x80, 0x80, 0x00, 0x70, 0x88, 0x88, 0x88, 0xa8, 0x90, 0x68, 0x00,
    0xf0, 0x88, 0x88, 0xf0, 0xa0, 0x90, 0x88, 0x00, 0x78, 0x80, 0x80, 0x70, 0x08, 0x08, 0xf0, 0x00,
    0xf8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00,
    0x88, 0x88, 0x88, 0x88, 0x88, 0x50, 0x20, 0x00, 0x88, 0x88, 0x88, 0xa8, 0xa8, 0xd8, 0x88, 0x00,
    0x88, 0x88, 0x50, 0x20, 0x50, 0x88, 0x88, 0x00, 0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x20, 0x00,
    0xf8, 0x08, 0x10, 0x20, 0x40, 0x80, 0xf8, 0x00, 0x70, 0x40, 0x40, 0x40, 0x40, 0x40, 0x70, 0x00,
    0x80, 0x40, 0x40, 0x20, 0x10, 0x10, 0x08, 0x00, 0x70, 0x10, 0x10, 0x10, 0x10, 0x10, 0x70, 0x00,
    0x20, 0x50, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8,
    0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x08, 0x78, 0x88, 0x78, 0x00,
    0x80, 0x80, 0xf0, 0x88, 0x88, 0x88, 0xf0, 0x00, 0x00, 0x00, 0x70, 0x80, 0x80, 0x80, 0x70, 0x00,
    0x08, 0x08, 0x78, 0x88, 0x88, 0x88, 0x78, 0x00, 0x00, 0x00, 0x70, 0x88, 0xf8, 0x80, 0x70, 0x00,
    0x30, 0x40, 0xf0, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x70,
    0x80, 0x80, 0xf0, 0x88, 0x88, 0x88, 0x88, 0x00, 0x20, 0x00, 0x60, 0x20, 0x20, 0x20, 0x70, 0x00,
    0x10, 0x00, 0x10, 0x10, 0x10, 0x10, 0x90, 0x60, 0x80, 0x80, 0x90, 0xa0, 0xc0, 0xa0, 0x90, 0x00,
    0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00, 0x00, 0x00, 0xd0, 0xa8, 0xa8, 0x88, 0x88, 0x00,
    0x00, 0x00, 0xf0, 0x88, 0x88, 0x88, 0x88, 0x00, 0x00, 0x00, 0x70, 0x88, 0x88, 0x88, 0x70, 0x00,
    0x00, 0x00, 0xf0, 0x88, 0x88, 0xf0, 0x80, 0x80, 0x00, 0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x08,
    0x00, 0x00, 0xb0, 0xc0, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x78, 0x80, 0x70, 0x08, 0xf0, 0x00,
    0x40, 0x40, 0xf0, 0x40, 0x40, 0x40, 0x30, 0x00, 0x00, 0x00, 0x88, 0x88, 0x88, 0x88, 0x78, 0x00,
    0x00, 0x00, 0x88, 0x88, 0x88, 0x50, 0x20, 0x00, 0x00, 0x00, 0x88, 0x88, 0xa8, 0xa8, 0x50, 0x00,
    0x00, 0x00, 0x88, 0x50, 0x20, 0x50, 0x88, 0x00, 0x00, 0x00, 0x88, 0x88, 0x88, 0x78, 0x08, 0x70,
    0x00, 0x00, 0xf8, 0x10, 0x20, 0x40, 0xf8, 0x00, 0x30, 0x20, 0x20, 0x40, 0x20, 0x20, 0x30, 0x00,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x60, 0x20, 0x20, 0x10, 0x20, 0x20, 0x60, 0x00,
    0x00, 0x48, 0xa8, 0x90, 0x00, 0x00, 0x00, 0x00
};

static Font defaultFont = { 0 };            // Default font, lazily built from fontDefaultData
static Image defaultFontAtlas = { 0 };      // Default font atlas kept in RAM to retry GPU upload
static int textLineSpacing = 2;             // Vertical line spacing added on '\n' (raylib default)

//----------------------------------------------------------------------------------
// Module internal functions declaration
//----------------------------------------------------------------------------------
static void DrawGlyphQuad(Texture2D texture, Rectangle src, Rectangle dst, Color tint);
static Vector2 MeasureTextInternal(Font font, const char *text, const int *codepoints, int length, float fontSize, float spacing);
static char *TextReplaceInternal(const char *text, const char *search, const char *replacement, char *dst, int dstSize);
static char *TextInsertInternal(const char *text, const char *insert, int position, char *dst, int dstSize);
static bool TextGetBetweenRange(const char *text, const char *begin, const char *end, int *start, int *length);

#if (defined(SUPPORT_FILEFORMAT_FNT) && SUPPORT_FILEFORMAT_FNT) || (defined(SUPPORT_FILEFORMAT_BDF) && SUPPORT_FILEFORMAT_BDF)
static int GetLine(const char *origin, char *buffer, int maxLength);    // Copy one text line into buffer, returns bytes read (without '\n')
#endif
#if defined(SUPPORT_FILEFORMAT_FNT) && SUPPORT_FILEFORMAT_FNT
static Font LoadBMFont(const char *fileName);       // Load a BMFont file (AngelCode font file)
#endif
#if defined(SUPPORT_FILEFORMAT_BDF) && SUPPORT_FILEFORMAT_BDF
static Font LoadBDFFont(const char *fileName);      // Load a BDF (Glyph Bitmap Distribution Format) font file
#endif

//----------------------------------------------------------------------------------
// Default font
//----------------------------------------------------------------------------------

// Build the embedded default font (idempotent), texture upload is retried while it fails
// NOTE: Exported so rcore can preload it on InitWindow(), see module header
void LoadFontDefault(void)
{
    if (defaultFont.glyphCount == 0)
    {
        GlyphInfo *glyphs = (GlyphInfo *)RL_CALLOC(FONT_DEFAULT_GLYPH_COUNT, sizeof(GlyphInfo));
        if (glyphs == NULL) return;

        for (int i = 0; i < FONT_DEFAULT_GLYPH_COUNT; i++)
        {
            glyphs[i].value = FONT_DEFAULT_FIRST_CHAR + i;
            glyphs[i].offsetX = 0;
            glyphs[i].offsetY = FONT_DEFAULT_GLYPH_OFFSET_Y;
            glyphs[i].advanceX = FONT_DEFAULT_ADVANCE_X;

            unsigned char *pixels = (unsigned char *)RL_CALLOC(FONT_DEFAULT_CELL_WIDTH*FONT_DEFAULT_CELL_HEIGHT, 1);

            if (pixels != NULL)
            {
                for (int y = 0; y < FONT_DEFAULT_CELL_HEIGHT; y++)
                {
                    unsigned char row = fontDefaultData[i*FONT_DEFAULT_CELL_HEIGHT + y];

                    for (int x = 0; x < FONT_DEFAULT_CELL_WIDTH; x++)
                    {
                        if ((row & (0x80 >> x)) != 0) pixels[y*FONT_DEFAULT_CELL_WIDTH + x] = 255;
                    }
                }
            }

            glyphs[i].image.data = pixels;
            glyphs[i].image.width = FONT_DEFAULT_CELL_WIDTH;
            glyphs[i].image.height = FONT_DEFAULT_CELL_HEIGHT;
            glyphs[i].image.mipmaps = 1;
            glyphs[i].image.format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
        }

        defaultFont.glyphs = glyphs;
        defaultFont.glyphCount = FONT_DEFAULT_GLYPH_COUNT;
        defaultFont.glyphPadding = FONT_DEFAULT_GLYPH_PADDING;
        defaultFont.baseSize = FONT_DEFAULT_BASE_SIZE;

        defaultFontAtlas = GenImageFontAtlas(defaultFont.glyphs, &defaultFont.recs, defaultFont.glyphCount,
                                             FONT_DEFAULT_BASE_SIZE, FONT_DEFAULT_GLYPH_PADDING, 0);

        TRACELOG(LOG_INFO, "FONT: Default font loaded successfully (%i glyphs)", defaultFont.glyphCount);
    }

    // Upload atlas to GPU, it can fail if no graphic device is ready yet (before InitWindow())
    if ((defaultFont.texture.id == 0) && (defaultFontAtlas.data != NULL))
    {
        defaultFont.texture = LoadTextureFromImage(defaultFontAtlas);

        if (defaultFont.texture.id != 0)
        {
            UnloadImage(defaultFontAtlas);
            defaultFontAtlas = (Image){ 0 };
        }
    }
}

// Release the default font resources (idempotent)
// NOTE: Exported so rcore can release it on CloseWindow(), see module header
void UnloadFontDefault(void)
{
    if (defaultFont.glyphCount == 0) return;

    UnloadFontData(defaultFont.glyphs, defaultFont.glyphCount);
    RL_FREE(defaultFont.recs);
    if (defaultFont.texture.id != 0) UnloadTexture(defaultFont.texture);
    if (defaultFontAtlas.data != NULL) UnloadImage(defaultFontAtlas);

    defaultFontAtlas = (Image){ 0 };
    defaultFont = (Font){ 0 };
}

// Get the default Font
Font GetFontDefault(void)
{
    if ((defaultFont.glyphCount == 0) || (defaultFont.texture.id == 0)) LoadFontDefault();

    return defaultFont;
}

//----------------------------------------------------------------------------------
// Font loading/unloading
//----------------------------------------------------------------------------------

// Load font from file into GPU memory (VRAM)
Font LoadFont(const char *fileName)
{
    Font font = { 0 };

    if (fileName == NULL) return GetFontDefault();

#if defined(SUPPORT_FILEFORMAT_TTF) && SUPPORT_FILEFORMAT_TTF
    if (IsFileExtension(fileName, ".ttf") || IsFileExtension(fileName, ".otf"))
    {
        font = LoadFontEx(fileName, FONT_TTF_DEFAULT_SIZE, NULL, 0);
    }
    else
#endif
#if defined(SUPPORT_FILEFORMAT_FNT) && SUPPORT_FILEFORMAT_FNT
    if (IsFileExtension(fileName, ".fnt")) font = LoadBMFont(fileName);
    else
#endif
#if defined(SUPPORT_FILEFORMAT_BDF) && SUPPORT_FILEFORMAT_BDF
    if (IsFileExtension(fileName, ".bdf")) font = LoadBDFFont(fileName);
    else
#endif
    {
        Image image = LoadImage(fileName);
        if (image.data != NULL) font = LoadFontFromImage(image, MAGENTA, FONT_TTF_DEFAULT_FIRST_CHAR);
        UnloadImage(image);
    }

    if (font.texture.id == 0)
    {
        TRACELOG(LOG_WARNING, "FONT: [%s] Failed to load font texture -> Using default font", fileName);
        font = GetFontDefault();
    }
    else
    {
        SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);    // By default, we set point filter (the best performance)
        TRACELOG(LOG_INFO, "FONT: Data loaded successfully (%i pixel size | %i glyphs)", font.baseSize, font.glyphCount);
    }

    return font;
}

// Load font from file with extended parameters
// NOTE: Requires TTF/OTF file data, use NULL codepoints and 0 count for the default charset
Font LoadFontEx(const char *fileName, int fontSize, const int *codepoints, int codepointCount)
{
    Font font = { 0 };

    if (fileName == NULL) return GetFontDefault();

    // Loading file to memory
    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);

    if (fileData != NULL)
    {
        // Loading font from memory data
        font = LoadFontFromMemory(GetFileExtension(fileName), fileData, dataSize, fontSize, codepoints, codepointCount);

        UnloadFileData(fileData);
    }
    else font = GetFontDefault();

    return font;
}

// Load font from a memory buffer, fileType refers to extension: i.e. ".ttf"
Font LoadFontFromMemory(const char *fileType, const unsigned char *fileData, int dataSize, int fontSize,
                        const int *codepoints, int codepointCount)
{
    Font font = { 0 };

    if ((fileType == NULL) || (fileData == NULL) || (dataSize <= 0) || (fontSize <= 0))
    {
        TRACELOG(LOG_WARNING, "FONT: Provided font data not valid -> Using default font");
        return GetFontDefault();
    }

    char fileExtLower[16] = { 0 };
    TextCopy(fileExtLower, TextToLower(fileType));
    fileExtLower[15] = '\0';

#if !(defined(SUPPORT_FILEFORMAT_TTF) && SUPPORT_FILEFORMAT_TTF)
    (void)codepoints; (void)codepointCount;     // Only used by the TTF/OTF path
#endif

#if defined(SUPPORT_FILEFORMAT_TTF) && SUPPORT_FILEFORMAT_TTF
    if (TextIsEqual(fileExtLower, ".ttf") || TextIsEqual(fileExtLower, ".otf"))
    {
        font.baseSize = fontSize;
        font.glyphCount = (codepointCount > 0)? codepointCount : FONT_TTF_DEFAULT_NUMCHARS;
        font.glyphPadding = 0;
        font.glyphs = LoadFontData(fileData, dataSize, font.baseSize, codepoints, font.glyphCount, FONT_DEFAULT, &font.glyphCount);

        if (font.glyphs != NULL)
        {
            font.glyphPadding = FONT_TTF_DEFAULT_CHARS_PADDING;

            Image atlas = GenImageFontAtlas(font.glyphs, &font.recs, font.glyphCount, font.baseSize, font.glyphPadding, 0);
            font.texture = LoadTextureFromImage(atlas);

            // Update glyphs[i].image to point to the atlas piece, glyph image data is no longer required
            for (int i = 0; i < font.glyphCount; i++)
            {
                UnloadImage(font.glyphs[i].image);
                font.glyphs[i].image = ImageFromImage(atlas, font.recs[i]);
            }

            UnloadImage(atlas);
        }
        else
        {
            TRACELOG(LOG_WARNING, "FONT: Failed to load font data from memory -> Using default font");
            font = GetFontDefault();
        }
    }
    else
#endif
    {
        TRACELOG(LOG_WARNING, "FONT: [%s] Font file type not supported from memory -> Using default font", fileExtLower);
        font = GetFontDefault();
    }

    return font;
}

// Check if a font is valid (font data loaded, WARNING: GPU texture not checked)
bool IsFontValid(Font font)
{
    return ((font.baseSize > 0) &&          // Validate font size
            (font.glyphCount > 0) &&        // Validate font contains some glyph
            (font.recs != NULL) &&          // Validate font recs defining glyphs on texture atlas
            (font.glyphs != NULL));         // Validate glyph data is loaded
}

// Load font data for further use, glyphs are rasterized to individual GRAYSCALE images
// NOTE: type is a FontType value: FONT_DEFAULT (anti-aliased), FONT_BITMAP (thresholded), FONT_SDF
GlyphInfo *LoadFontData(const unsigned char *fileData, int dataSize, int fontSize, const int *codepoints,
                        int codepointCount, int type, int *glyphCount)
{
    GlyphInfo *chars = NULL;

    if (glyphCount != NULL) *glyphCount = 0;

#if defined(SUPPORT_FILEFORMAT_TTF) && SUPPORT_FILEFORMAT_TTF
    if ((fileData == NULL) || (dataSize <= 0) || (fontSize <= 0)) return NULL;

    stbtt_fontinfo fontInfo = { 0 };

    if (stbtt_InitFont(&fontInfo, (const unsigned char *)fileData, 0) == 0)
    {
        TRACELOG(LOG_WARNING, "FONT: Failed to process TTF font data");
        return NULL;
    }

    // Calculate font scale factor and vertical metrics
    float scaleFactor = stbtt_ScaleForPixelHeight(&fontInfo, (float)fontSize);

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);

    // In case no chars count provided, default to 95
    codepointCount = (codepointCount > 0)? codepointCount : FONT_TTF_DEFAULT_NUMCHARS;

    // Fill codepoints in case they are not provided externally
    bool genFontChars = false;
    int *codepointsInternal = (int *)codepoints;

    if (codepoints == NULL)
    {
        codepointsInternal = (int *)RL_CALLOC(codepointCount, sizeof(int));
        if (codepointsInternal == NULL) return NULL;

        for (int i = 0; i < codepointCount; i++) codepointsInternal[i] = i + FONT_TTF_DEFAULT_FIRST_CHAR;

        genFontChars = true;
    }

    chars = (GlyphInfo *)RL_CALLOC(codepointCount, sizeof(GlyphInfo));

    if (chars == NULL)
    {
        if (genFontChars) RL_FREE(codepointsInternal);
        return NULL;
    }

    // NOTE: Every glyph is rasterized to its own image, packing happens on GenImageFontAtlas()
    for (int i = 0; i < codepointCount; i++)
    {
        int chw = 0, chh = 0;               // Glyph width and height (on generation)
        int ch = codepointsInternal[i];     // Character value to get info for

        chars[i].value = ch;

        // Render a unicode codepoint to a bitmap
        //   - stbtt_GetCodepointBitmap() -> allocates and returns a bitmap
        //   - stbtt_GetCodepointSDF()    -> allocates and returns a signed distance field
        if (type != FONT_SDF) chars[i].image.data = stbtt_GetCodepointBitmap(&fontInfo, scaleFactor, scaleFactor, ch, &chw, &chh, &chars[i].offsetX, &chars[i].offsetY);
        else if (ch != 32) chars[i].image.data = stbtt_GetCodepointSDF(&fontInfo, scaleFactor, ch, FONT_SDF_CHAR_PADDING, FONT_SDF_ON_EDGE_VALUE, FONT_SDF_PIXEL_DIST_SCALE, &chw, &chh, &chars[i].offsetX, &chars[i].offsetY);
        else chars[i].image.data = NULL;

        stbtt_GetCodepointHMetrics(&fontInfo, ch, &chars[i].advanceX, NULL);
        chars[i].advanceX = (int)((float)chars[i].advanceX*scaleFactor);

        chars[i].image.width = chw;
        chars[i].image.height = chh;
        chars[i].image.mipmaps = 1;
        chars[i].image.format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;

        chars[i].offsetY += (int)((float)ascent*scaleFactor);

        // NOTE: We create an empty image for the space character, it could be further required for atlas packing
        if (ch == 32)
        {
            RL_FREE(chars[i].image.data);

            int spaceWidth = (chars[i].advanceX > 0)? chars[i].advanceX : (fontSize/2);
            Image imSpace = {
                .data = RL_CALLOC(spaceWidth*fontSize, 1),
                .width = spaceWidth,
                .height = fontSize,
                .mipmaps = 1,
                .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE
            };

            chars[i].image = imSpace;
        }

        if ((type == FONT_BITMAP) && (chars[i].image.data != NULL))
        {
            // Aliased bitmap (black & white) font generation, avoiding anti-aliasing
            unsigned char *pixels = (unsigned char *)chars[i].image.data;

            for (int p = 0; p < chars[i].image.width*chars[i].image.height; p++)
            {
                pixels[p] = (pixels[p] < FONT_BITMAP_ALPHA_THRESHOLD)? 0 : 255;
            }
        }
    }

    if (genFontChars) RL_FREE(codepointsInternal);

    if (glyphCount != NULL) *glyphCount = codepointCount;
#else
    (void)fileData; (void)dataSize; (void)fontSize; (void)codepoints; (void)codepointCount; (void)type;
    TRACELOG(LOG_WARNING, "FONT: TTF font support not compiled in (SUPPORT_FILEFORMAT_TTF)");
#endif

    return chars;
}

// Generate image font atlas using chars info
// NOTE: Packing method: 0-Default (basic grid packing), 1-Skyline (stb_rect_pack)
Image GenImageFontAtlas(const GlyphInfo *glyphs, Rectangle **glyphRecs, int glyphCount, int fontSize, int padding, int packMethod)
{
    Image atlas = { 0 };

    if (glyphRecs != NULL) *glyphRecs = NULL;

    if ((glyphs == NULL) || (glyphRecs == NULL))
    {
        TRACELOG(LOG_WARNING, "FONT: Provided chars info not valid, returning empty image atlas");
        return atlas;
    }

    // In case no chars count provided we suppose default of 95
    glyphCount = (glyphCount > 0)? glyphCount : FONT_TTF_DEFAULT_NUMCHARS;
    fontSize = (fontSize > 0)? fontSize : FONT_TTF_DEFAULT_SIZE;
    padding = (padding > 0)? padding : 0;

    // NOTE: Rectangles memory is loaded here!
    Rectangle *recs = (Rectangle *)RL_CALLOC(glyphCount, sizeof(Rectangle));
    if (recs == NULL) return atlas;

    // Calculate image size based on total glyph width and glyph row count
    int totalWidth = 0;
    int maxGlyphWidth = 0;
    int maxGlyphHeight = fontSize;

    for (int i = 0; i < glyphCount; i++)
    {
        if (glyphs[i].image.width > maxGlyphWidth) maxGlyphWidth = glyphs[i].image.width;
        if (glyphs[i].image.height > maxGlyphHeight) maxGlyphHeight = glyphs[i].image.height;
        totalWidth += glyphs[i].image.width + 2*padding;
    }

    // Calculate space for glyphs in image, doubling image size until everything fits (POT sizes)
    // NOTE: maxGlyphWidth is the maximum possible space left unused at the end of a row
    int rowCount = 0;
    int imageSize = 64;

    while (totalWidth > (imageSize - maxGlyphWidth)*rowCount)
    {
        imageSize *= 2;
        rowCount = imageSize/(maxGlyphHeight + 2*padding);

        if (imageSize >= 16384) break;      // Safety limit: 16K x 16K atlas
    }

    atlas.width = imageSize;
    atlas.height = imageSize;
    atlas.mipmaps = 1;
    atlas.format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
    atlas.data = (unsigned char *)RL_CALLOC(1, atlas.width*atlas.height);

    if (atlas.data == NULL)
    {
        RL_FREE(recs);
        atlas = (Image){ 0 };
        return atlas;
    }

    if (packMethod == 0)    // Use basic packing algorithm: one glyph after another, row by row
    {
        int offsetX = padding;
        int offsetY = padding;

        for (int i = 0; i < glyphCount; i++)
        {
            // Check remaining space for the glyph on the current row
            if (offsetX >= (atlas.width - glyphs[i].image.width - 2*padding))
            {
                offsetX = padding;
                offsetY += (maxGlyphHeight + 2*padding);

                if (offsetY > (atlas.height - maxGlyphHeight - padding))
                {
                    for (int j = i; j < glyphCount; j++)
                    {
                        TRACELOG(LOG_WARNING, "FONT: Failed to package character (%i)", j);
                        recs[j] = (Rectangle){ 0.0f, 0.0f, 0.0f, 0.0f };
                    }
                    break;
                }
            }

            // Copy pixel data from glyph image to atlas
            if (glyphs[i].image.data != NULL)
            {
                for (int y = 0; y < glyphs[i].image.height; y++)
                {
                    for (int x = 0; x < glyphs[i].image.width; x++)
                    {
                        ((unsigned char *)atlas.data)[(offsetY + y)*atlas.width + (offsetX + x)] =
                            ((unsigned char *)glyphs[i].image.data)[y*glyphs[i].image.width + x];
                    }
                }
            }

            recs[i] = (Rectangle){ (float)offsetX, (float)offsetY, (float)glyphs[i].image.width, (float)glyphs[i].image.height };

            offsetX += (glyphs[i].image.width + 2*padding);
        }
    }
    else                    // Use skyline rect packing algorithm (stb_rect_pack)
    {
        stbrp_context *context = (stbrp_context *)RL_CALLOC(1, sizeof(stbrp_context));
        stbrp_node *nodes = (stbrp_node *)RL_CALLOC(glyphCount, sizeof(stbrp_node));
        stbrp_rect *rects = (stbrp_rect *)RL_CALLOC(glyphCount, sizeof(stbrp_rect));

        if ((context == NULL) || (nodes == NULL) || (rects == NULL))
        {
            RL_FREE(context); RL_FREE(nodes); RL_FREE(rects); RL_FREE(recs);
            RL_FREE(atlas.data);
            atlas = (Image){ 0 };
            return atlas;
        }

        stbrp_init_target(context, atlas.width, atlas.height, nodes, glyphCount);

        for (int i = 0; i < glyphCount; i++)
        {
            rects[i].id = i;
            rects[i].w = (stbrp_coord)(glyphs[i].image.width + 2*padding);
            rects[i].h = (stbrp_coord)(glyphs[i].image.height + 2*padding);
        }

        stbrp_pack_rects(context, rects, glyphCount);

        for (int i = 0; i < glyphCount; i++)
        {
            // It returns char rectangles in the atlas, we need to adjust for padding
            recs[i].x = (float)(rects[i].x + padding);
            recs[i].y = (float)(rects[i].y + padding);
            recs[i].width = (float)glyphs[i].image.width;
            recs[i].height = (float)glyphs[i].image.height;

            if (!rects[i].was_packed)
            {
                TRACELOG(LOG_WARNING, "FONT: Failed to package character (%i)", i);
                recs[i] = (Rectangle){ 0.0f, 0.0f, 0.0f, 0.0f };
                continue;
            }

            if (glyphs[i].image.data == NULL) continue;

            for (int y = 0; y < glyphs[i].image.height; y++)
            {
                for (int x = 0; x < glyphs[i].image.width; x++)
                {
                    ((unsigned char *)atlas.data)[(rects[i].y + padding + y)*atlas.width + (rects[i].x + padding + x)] =
                        ((unsigned char *)glyphs[i].image.data)[y*glyphs[i].image.width + x];
                }
            }
        }

        RL_FREE(rects);
        RL_FREE(nodes);
        RL_FREE(context);
    }

    // Convert image data from GRAYSCALE to GRAY_ALPHA, white pixels with glyph coverage on alpha
    unsigned char *dataGrayAlpha = (unsigned char *)RL_MALLOC(atlas.width*atlas.height*2);

    if (dataGrayAlpha != NULL)
    {
        for (int i = 0, k = 0; i < atlas.width*atlas.height; i++, k += 2)
        {
            dataGrayAlpha[k] = 255;
            dataGrayAlpha[k + 1] = ((unsigned char *)atlas.data)[i];
        }

        RL_FREE(atlas.data);
        atlas.data = dataGrayAlpha;
        atlas.format = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
    }

    *glyphRecs = recs;

    return atlas;
}

// Unload font chars info data (RAM)
void UnloadFontData(GlyphInfo *glyphs, int glyphCount)
{
    if (glyphs == NULL) return;

    for (int i = 0; i < glyphCount; i++) UnloadImage(glyphs[i].image);

    RL_FREE(glyphs);
}

// Unload font from GPU memory (VRAM)
void UnloadFont(Font font)
{
    // NOTE: Make sure the default font is not unloaded by the user, it is owned by this module
    if ((font.texture.id != 0) && (font.texture.id == defaultFont.texture.id)) return;

    UnloadFontData(font.glyphs, font.glyphCount);
    RL_FREE(font.recs);
    if (font.texture.id != 0) UnloadTexture(font.texture);
}

// Load font from Image (XNA style)
// NOTE: Glyphs are separated by 'key' colored lines, first line/column define spacing
Font LoadFontFromImage(Image image, Color key, int firstChar)
{
    #ifndef MAX_GLYPHS_FROM_IMAGE
        #define MAX_GLYPHS_FROM_IMAGE   256     // Maximum number of glyphs supported on image scan
    #endif

    Font font = GetFontDefault();

    if ((image.data == NULL) || (image.width <= 0) || (image.height <= 0))
    {
        TRACELOG(LOG_WARNING, "FONT: Provided image not valid -> Using default font");
        return font;
    }

    Color *pixels = LoadImageColors(image);
    if (pixels == NULL) return font;

    // Parse image data to get charSpacing and lineSpacing: position of the first non-key pixel
    int x = 0, y = 0;
    bool found = false;

    for (y = 0; (y < image.height) && !found; y++)
    {
        for (x = 0; x < image.width; x++)
        {
            if (!TEXT_COLOR_EQUAL(pixels[y*image.width + x], key)) { found = true; break; }
        }
    }
    if (found) y--;     // Compensate the outer loop increment

    if (!found || (x == 0) || (y == 0))
    {
        TRACELOG(LOG_WARNING, "FONT: Image font parsing failed, no glyphs found -> Using default font");
        UnloadImageColors(pixels);
        return font;
    }

    int charSpacing = x;
    int lineSpacing = y;

    // Get the height of the first glyph: scan down until the key color is found again
    int charHeight = 0;
    while (((lineSpacing + charHeight) < image.height) &&
           !TEXT_COLOR_EQUAL(pixels[(lineSpacing + charHeight)*image.width + charSpacing], key)) charHeight++;

    if (charHeight == 0)
    {
        TRACELOG(LOG_WARNING, "FONT: Image font parsing failed, glyph height is zero -> Using default font");
        UnloadImageColors(pixels);
        return font;
    }

    // Scan the image to get every glyph rectangle: value, x, y, width, height
    int tempCharValues[MAX_GLYPHS_FROM_IMAGE] = { 0 };
    Rectangle tempCharRecs[MAX_GLYPHS_FROM_IMAGE] = { { 0 } };
    int index = 0;
    int lineToRead = 0;
    int xPosToRead = charSpacing;

    while (((lineSpacing + lineToRead*(charHeight + lineSpacing)) < image.height) && (index < MAX_GLYPHS_FROM_IMAGE))
    {
        int lineY = lineSpacing + (charHeight + lineSpacing)*lineToRead;

        while ((xPosToRead < image.width) && (index < MAX_GLYPHS_FROM_IMAGE) &&
               !TEXT_COLOR_EQUAL(pixels[lineY*image.width + xPosToRead], key))
        {
            int charWidth = 0;
            while (((xPosToRead + charWidth) < image.width) &&
                   !TEXT_COLOR_EQUAL(pixels[lineY*image.width + xPosToRead + charWidth], key)) charWidth++;

            tempCharValues[index] = firstChar + index;
            tempCharRecs[index] = (Rectangle){ (float)xPosToRead, (float)lineY, (float)charWidth, (float)charHeight };
            index++;

            xPosToRead += (charWidth + charSpacing);
        }

        lineToRead++;
        xPosToRead = charSpacing;
    }

    UnloadImageColors(pixels);

    if (index == 0)
    {
        TRACELOG(LOG_WARNING, "FONT: Image font parsing failed, no glyphs found -> Using default font");
        return font;
    }

    // NOTE: We remove the key color from the image to avoid weird artifacts on texture filtering
    Image fontClear = ImageCopy(image);
    ImageFormat(&fontClear, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    ImageColorReplace(&fontClear, key, BLANK);

    font = (Font){ 0 };
    font.texture = LoadTextureFromImage(fontClear);
    font.glyphCount = index;
    font.glyphPadding = 0;
    font.glyphs = (GlyphInfo *)RL_CALLOC(font.glyphCount, sizeof(GlyphInfo));
    font.recs = (Rectangle *)RL_CALLOC(font.glyphCount, sizeof(Rectangle));

    if ((font.glyphs == NULL) || (font.recs == NULL))
    {
        RL_FREE(font.glyphs);
        RL_FREE(font.recs);
        if (font.texture.id != 0) UnloadTexture(font.texture);
        UnloadImage(fontClear);
        return GetFontDefault();
    }

    for (int i = 0; i < font.glyphCount; i++)
    {
        font.glyphs[i].value = tempCharValues[i];
        font.recs[i] = tempCharRecs[i];

        // NOTE: On image based fonts (XNA style), character offsets and xAdvance are not required
        font.glyphs[i].offsetX = 0;
        font.glyphs[i].offsetY = 0;
        font.glyphs[i].advanceX = 0;

        font.glyphs[i].image = ImageFromImage(fontClear, tempCharRecs[i]);
    }

    UnloadImage(fontClear);

    font.baseSize = (int)font.recs[0].height;

    TRACELOG(LOG_INFO, "FONT: Image font loaded successfully (%i glyphs)", font.glyphCount);

    return font;
}

// Export font as code file (.h), replicating raylib generated file structure
bool ExportFontAsCode(Font font, const char *fileName)
{
    #ifndef TEXT_BYTES_PER_LINE
        #define TEXT_BYTES_PER_LINE     20
    #endif

    bool success = false;

    if (!IsFontValid(font) || (fileName == NULL))
    {
        TRACELOG(LOG_WARNING, "FONT: Provided font or file name not valid, unable to export font as code");
        return false;
    }

    // Get file name in PascalCase to compose the generated identifiers
    char fileNamePascal[256] = { 0 };
    TextCopy(fileNamePascal, TextSubtext(TextToPascal(GetFileNameWithoutExt(fileName)), 0, 255));

    // Make sure the composed identifiers are valid C identifiers
    for (int i = 0; fileNamePascal[i] != '\0'; i++)
    {
        char c = fileNamePascal[i];
        bool valid = (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_') || ((c >= '0') && (c <= '9') && (i > 0)));
        if (!valid) fileNamePascal[i] = '_';
    }
    if (fileNamePascal[0] == '\0') TextCopy(fileNamePascal, "Font");

    Image image = LoadImageFromTexture(font.texture);

    if (image.data == NULL)
    {
        TRACELOG(LOG_WARNING, "FONT: [%s] Failed to retrieve font atlas image from GPU", fileName);
        return false;
    }

    if (image.format != PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA)
    {
        TRACELOG(LOG_WARNING, "FONT: [%s] Font image format is not GRAY+ALPHA", fileName);
    }

    int imageDataSize = GetPixelDataSize(image.width, image.height, image.format);

    // Compress font atlas pixel data, it must be decompressed with DecompressData()
    int compDataSize = 0;
    unsigned char *compData = CompressData((const unsigned char *)image.data, imageDataSize, &compDataSize);

    if ((compData == NULL) || (compDataSize <= 0))
    {
        TRACELOG(LOG_WARNING, "FONT: [%s] Failed to compress font atlas image data", fileName);
        UnloadImage(image);
        return false;
    }

    // NOTE: Text buffer size is computed instead of using a fixed 1 MB buffer as raylib does:
    // every compressed byte requires up to 8 chars ("0x00,\n    ") and every glyph up to 96 chars
    int txtDataSize = 4096 + compDataSize*8 + font.glyphCount*96 + (int)sizeof(fileNamePascal)*16;
    char *txtData = (char *)RL_CALLOC(txtDataSize, 1);

    if (txtData == NULL)
    {
        MemFree(compData);
        UnloadImage(image);
        return false;
    }

    int byteCount = 0;

    byteCount += sprintf(txtData + byteCount, "//////////////////////////////////////////////////////////////////////////////////\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                              //\n");
    byteCount += sprintf(txtData + byteCount, "// FontAsCode exporter v1.0 - Font data exported as an array of bytes           //\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                              //\n");
    byteCount += sprintf(txtData + byteCount, "// more info and bugs-report:  github.com/raysan5/raylib                        //\n");
    byteCount += sprintf(txtData + byteCount, "// feedback and support:       ray[at]raylib.com                                //\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                              //\n");
    byteCount += sprintf(txtData + byteCount, "// Copyright (c) 2025 Ramon Santamaria (@raysan5)                               //\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                              //\n");
    byteCount += sprintf(txtData + byteCount, "//////////////////////////////////////////////////////////////////////////////////\n\n");

    byteCount += sprintf(txtData + byteCount, "// Font glyphs rectangles data (on atlas)\n");
    byteCount += sprintf(txtData + byteCount, "#define COMPRESSED_DATA_SIZE_FONT_%s %i\n\n", TextToUpper(fileNamePascal), compDataSize);

    byteCount += sprintf(txtData + byteCount, "// Font image pixels data compressed (DEFLATE)\n");
    byteCount += sprintf(txtData + byteCount, "// NOTE: Original pixel data is GRAYSCALE + ALPHA\n");
    byteCount += sprintf(txtData + byteCount, "static unsigned char fontData_%s[COMPRESSED_DATA_SIZE_FONT_%s] = { ", fileNamePascal, TextToUpper(fileNamePascal));
    for (int i = 0; i < (compDataSize - 1); i++) byteCount += sprintf(txtData + byteCount, ((i%TEXT_BYTES_PER_LINE == 0)? "0x%02x,\n    " : "0x%02x, "), compData[i]);
    byteCount += sprintf(txtData + byteCount, "0x%02x };\n\n", compData[compDataSize - 1]);

    MemFree(compData);

    // Save font recs data
    byteCount += sprintf(txtData + byteCount, "// Font characters rectangles data\n");
    byteCount += sprintf(txtData + byteCount, "static const Rectangle fontRecs_%s[%i] = {\n", fileNamePascal, font.glyphCount);
    for (int i = 0; i < font.glyphCount; i++)
    {
        byteCount += sprintf(txtData + byteCount, "    { %1.0f, %1.0f, %1.0f , %1.0f },\n",
                             (double)font.recs[i].x, (double)font.recs[i].y, (double)font.recs[i].width, (double)font.recs[i].height);
    }
    byteCount += sprintf(txtData + byteCount, "};\n\n");

    // Save font glyphs data
    // NOTE: No need to save glyphs image data, it can be generated from image and recs
    byteCount += sprintf(txtData + byteCount, "// Font glyphs info data\n");
    byteCount += sprintf(txtData + byteCount, "// NOTE: No glyphs.image data provided\n");
    byteCount += sprintf(txtData + byteCount, "static const GlyphInfo fontGlyphs_%s[%i] = {\n", fileNamePascal, font.glyphCount);
    for (int i = 0; i < font.glyphCount; i++)
    {
        byteCount += sprintf(txtData + byteCount, "    { %i, %i, %i, %i, { 0 }},\n",
                             font.glyphs[i].value, font.glyphs[i].offsetX, font.glyphs[i].offsetY, font.glyphs[i].advanceX);
    }
    byteCount += sprintf(txtData + byteCount, "};\n\n");

    // Custom font loading function
    byteCount += sprintf(txtData + byteCount, "// Font loading function: %s\n", fileNamePascal);
    byteCount += sprintf(txtData + byteCount, "static Font LoadFont%s(void)\n{\n", fileNamePascal);
    byteCount += sprintf(txtData + byteCount, "    Font font = { 0 };\n\n");
    byteCount += sprintf(txtData + byteCount, "    font.baseSize = %i;\n", font.baseSize);
    byteCount += sprintf(txtData + byteCount, "    font.glyphCount = %i;\n", font.glyphCount);
    byteCount += sprintf(txtData + byteCount, "    font.glyphPadding = %i;\n\n", font.glyphPadding);
    byteCount += sprintf(txtData + byteCount, "    // Custom font loading\n");
    byteCount += sprintf(txtData + byteCount, "    // NOTE: Compressed font image data (DEFLATE), it requires DecompressData() function\n");
    byteCount += sprintf(txtData + byteCount, "    int fontDataSize_%s = 0;\n", fileNamePascal);
    byteCount += sprintf(txtData + byteCount, "    unsigned char *data = DecompressData(fontData_%s, COMPRESSED_DATA_SIZE_FONT_%s, &fontDataSize_%s);\n",
                         fileNamePascal, TextToUpper(fileNamePascal), fileNamePascal);
    byteCount += sprintf(txtData + byteCount, "    Image imFont = { data, %i, %i, 1, %i };\n\n", image.width, image.height, image.format);
    byteCount += sprintf(txtData + byteCount, "    // Load texture from image\n");
    byteCount += sprintf(txtData + byteCount, "    font.texture = LoadTextureFromImage(imFont);\n");
    byteCount += sprintf(txtData + byteCount, "    UnloadImage(imFont);  // Uncompressed data can be unloaded from memory\n\n");
    byteCount += sprintf(txtData + byteCount, "    // Assign glyph recs and info data directly\n");
    byteCount += sprintf(txtData + byteCount, "    // WARNING: This font data must not be unloaded\n");
    byteCount += sprintf(txtData + byteCount, "    font.recs = (Rectangle *)fontRecs_%s;\n", fileNamePascal);
    byteCount += sprintf(txtData + byteCount, "    font.glyphs = (GlyphInfo *)fontGlyphs_%s;\n\n", fileNamePascal);
    byteCount += sprintf(txtData + byteCount, "    return font;\n}\n");

    UnloadImage(image);

    // NOTE: Text data length exported is determined by '\0' (NULL) character
    success = SaveFileText(fileName, txtData);

    RL_FREE(txtData);

    if (success) TRACELOG(LOG_INFO, "FONT: [%s] Font as code exported successfully", fileName);
    else TRACELOG(LOG_WARNING, "FONT: [%s] Failed to export font as code", fileName);

    return success;
}

//----------------------------------------------------------------------------------
// Text drawing
//----------------------------------------------------------------------------------

// Draw current FPS, color tinted depending on the frame rate
void DrawFPS(int posX, int posY)
{
    Color color = LIME;                     // Good FPS
    int fps = GetFPS();

    if ((fps < 30) && (fps >= 15)) color = ORANGE;  // Warning FPS
    else if (fps < 15) color = RED;                 // Low FPS

    DrawText(TextFormat("%2i FPS", fps), posX, posY, 20, color);
}

// Draw text (using default font)
// NOTE: fontSize is clamped to the default font base size, spacing scales with it
void DrawText(const char *text, int posX, int posY, int fontSize, Color color)
{
    Font font = GetFontDefault();

    if (!IsFontValid(font)) return;

    int defaultFontSize = FONT_DEFAULT_BASE_SIZE;
    if (fontSize < defaultFontSize) fontSize = defaultFontSize;
    int spacing = fontSize/defaultFontSize;

    DrawTextEx(font, text, (Vector2){ (float)posX, (float)posY }, (float)fontSize, (float)spacing, color);
}

// Draw text using Font and additional parameters
void DrawTextEx(Font font, const char *text, Vector2 position, float fontSize, float spacing, Color tint)
{
    if ((text == NULL) || (font.texture.id == 0) || !IsFontValid(font)) return;

    int size = (int)TextLength(text);       // Total size in bytes of the text, scanned by codepoints in loop

    float textOffsetY = 0.0f;               // Offset between lines (on linebreak '\n')
    float textOffsetX = 0.0f;               // Offset X to next character to draw

    float scaleFactor = fontSize/(float)font.baseSize;

    for (int i = 0; i < size;)
    {
        // Get next codepoint from byte string and glyph index in font
        int codepointByteCount = 0;
        int codepoint = GetCodepointNext(&text[i], &codepointByteCount);
        int index = GetGlyphIndex(font, codepoint);

        if (codepoint == '\n')
        {
            // NOTE: Line spacing is a global variable, use SetTextLineSpacing() to setup
            textOffsetY += (fontSize + textLineSpacing);
            textOffsetX = 0.0f;
        }
        else
        {
            if ((codepoint != ' ') && (codepoint != '\t'))
            {
                DrawTextCodepoint(font, codepoint, (Vector2){ position.x + textOffsetX, position.y + textOffsetY }, fontSize, tint);
            }

            if (font.glyphs[index].advanceX == 0) textOffsetX += ((float)font.recs[index].width*scaleFactor + spacing);
            else textOffsetX += ((float)font.glyphs[index].advanceX*scaleFactor + spacing);
        }

        i += codepointByteCount;    // Move text bytes counter to next codepoint
    }
}

// Draw text using Font and pro parameters (rotation)
void DrawTextPro(Font font, const char *text, Vector2 position, Vector2 origin, float rotation, float fontSize, float spacing, Color tint)
{
    rlPushMatrix();

        rlTranslatef(position.x, position.y, 0.0f);
        rlRotatef(rotation, 0.0f, 0.0f, 1.0f);
        rlTranslatef(-origin.x, -origin.y, 0.0f);

        DrawTextEx(font, text, (Vector2){ 0.0f, 0.0f }, fontSize, spacing, tint);

    rlPopMatrix();
}

// Draw one character (codepoint)
// NOTE: Emitted as a single textured quad on the rlvk batch (no DrawTexture* usage)
void DrawTextCodepoint(Font font, int codepoint, Vector2 position, float fontSize, Color tint)
{
    if (!IsFontValid(font) || (font.texture.id == 0)) return;

    // Character index position in sprite font, fallback to '?' if not found
    int index = GetGlyphIndex(font, codepoint);
    float scaleFactor = fontSize/(float)font.baseSize;

    // Character destination rectangle on screen
    // NOTE: We consider glyphPadding on drawing
    Rectangle dstRec = {
        position.x + font.glyphs[index].offsetX*scaleFactor - (float)font.glyphPadding*scaleFactor,
        position.y + font.glyphs[index].offsetY*scaleFactor - (float)font.glyphPadding*scaleFactor,
        (font.recs[index].width + 2.0f*font.glyphPadding)*scaleFactor,
        (font.recs[index].height + 2.0f*font.glyphPadding)*scaleFactor
    };

    // Character source rectangle from font texture atlas
    // NOTE: We consider chars padding when drawing, it could be zero
    Rectangle srcRec = {
        font.recs[index].x - (float)font.glyphPadding,
        font.recs[index].y - (float)font.glyphPadding,
        font.recs[index].width + 2.0f*font.glyphPadding,
        font.recs[index].height + 2.0f*font.glyphPadding
    };

    if ((srcRec.width <= 0.0f) || (srcRec.height <= 0.0f)) return;      // Nothing to draw (unpacked glyph)

    DrawGlyphQuad(font.texture, srcRec, dstRec, tint);
}

// Draw multiple characters (codepoints)
void DrawTextCodepoints(Font font, const int *codepoints, int codepointCount, Vector2 position, float fontSize, float spacing, Color tint)
{
    if ((codepoints == NULL) || (codepointCount <= 0) || !IsFontValid(font)) return;

    float textOffsetY = 0.0f;
    float textOffsetX = 0.0f;
    float scaleFactor = fontSize/(float)font.baseSize;

    for (int i = 0; i < codepointCount; i++)
    {
        int index = GetGlyphIndex(font, codepoints[i]);

        if (codepoints[i] == '\n')
        {
            textOffsetY += (fontSize + textLineSpacing);
            textOffsetX = 0.0f;
        }
        else
        {
            if ((codepoints[i] != ' ') && (codepoints[i] != '\t'))
            {
                DrawTextCodepoint(font, codepoints[i], (Vector2){ position.x + textOffsetX, position.y + textOffsetY }, fontSize, tint);
            }

            if (font.glyphs[index].advanceX == 0) textOffsetX += ((float)font.recs[index].width*scaleFactor + spacing);
            else textOffsetX += ((float)font.glyphs[index].advanceX*scaleFactor + spacing);
        }
    }
}

//----------------------------------------------------------------------------------
// Text font info
//----------------------------------------------------------------------------------

// Set vertical line spacing when drawing with line-breaks
void SetTextLineSpacing(int spacing)
{
    textLineSpacing = spacing;
}

// Measure string width for default font
int MeasureText(const char *text, int fontSize)
{
    Font font = GetFontDefault();

    if (!IsFontValid(font)) return 0;

    int defaultFontSize = FONT_DEFAULT_BASE_SIZE;
    if (fontSize < defaultFontSize) fontSize = defaultFontSize;
    int spacing = fontSize/defaultFontSize;

    Vector2 textSize = MeasureTextEx(font, text, (float)fontSize, (float)spacing);

    return (int)textSize.x;
}

// Measure string size for Font
Vector2 MeasureTextEx(Font font, const char *text, float fontSize, float spacing)
{
    if ((text == NULL) || (text[0] == '\0')) return (Vector2){ 0.0f, 0.0f };

    return MeasureTextInternal(font, text, NULL, 0, fontSize, spacing);
}

// Measure string size for an existing array of codepoints for Font
Vector2 MeasureTextCodepoints(Font font, const int *codepoints, int length, float fontSize, float spacing)
{
    if ((codepoints == NULL) || (length <= 0)) return (Vector2){ 0.0f, 0.0f };

    return MeasureTextInternal(font, NULL, codepoints, length, fontSize, spacing);
}

// Get glyph index position in font for a codepoint (unicode character), fallback to '?' if not found
int GetGlyphIndex(Font font, int codepoint)
{
    if (!IsFontValid(font)) return 0;

    int index = 0;
    int fallbackIndex = 0;      // Get index of fallback glyph '?'

    // Look for the character index in the (possibly unordered) charset
    for (int i = 0; i < font.glyphCount; i++)
    {
        if (font.glyphs[i].value == GLYPH_NOTFOUND_CHAR_FALLBACK) fallbackIndex = i;

        if (font.glyphs[i].value == codepoint) { index = i; break; }
    }

    if ((index == 0) && (font.glyphs[0].value != codepoint)) index = fallbackIndex;

    return index;
}

// Get glyph font info data for a codepoint (unicode character), fallback to '?' if not found
GlyphInfo GetGlyphInfo(Font font, int codepoint)
{
    if (!IsFontValid(font)) return (GlyphInfo){ 0 };

    return font.glyphs[GetGlyphIndex(font, codepoint)];
}

// Get glyph rectangle in font atlas for a codepoint (unicode character), fallback to '?' if not found
Rectangle GetGlyphAtlasRec(Font font, int codepoint)
{
    if (!IsFontValid(font)) return (Rectangle){ 0.0f, 0.0f, 0.0f, 0.0f };

    return font.recs[GetGlyphIndex(font, codepoint)];
}

//----------------------------------------------------------------------------------
// Text codepoints management (unicode characters)
//----------------------------------------------------------------------------------

// Load UTF-8 text encoded from codepoints array
// WARNING: Allocated memory must be manually freed with UnloadUTF8()
char *LoadUTF8(const int *codepoints, int length)
{
    if ((codepoints == NULL) || (length <= 0)) return NULL;

    // We allocate enough memory to fit all possible codepoints
    // NOTE: 5 bytes for every codepoint should be enough
    char *text = (char *)RL_CALLOC(length*5, 1);
    if (text == NULL) return NULL;

    int size = 0;

    for (int i = 0; i < length; i++)
    {
        int bytes = 0;
        const char *utf8 = CodepointToUTF8(codepoints[i], &bytes);
        memcpy(text + size, utf8, bytes);
        size += bytes;
    }

    // Resize memory to text length + string NULL terminator
    void *ptr = RL_REALLOC(text, size + 1);
    if (ptr != NULL) text = (char *)ptr;

    return text;
}

// Unload UTF-8 text encoded from codepoints array
void UnloadUTF8(char *text)
{
    RL_FREE(text);
}

// Load all codepoints from a UTF-8 text string, codepoints count returned by parameter
int *LoadCodepoints(const char *text, int *count)
{
    if (count != NULL) *count = 0;
    if ((text == NULL) || (text[0] == '\0')) return NULL;

    int textLength = (int)TextLength(text);

    // Allocate a big enough buffer to store as many codepoints as text bytes
    int *codepoints = (int *)RL_CALLOC(textLength, sizeof(int));
    if (codepoints == NULL) return NULL;

    int codepointCount = 0;

    for (int i = 0; i < textLength; codepointCount++)
    {
        int codepointSize = 0;
        codepoints[codepointCount] = GetCodepointNext(text + i, &codepointSize);
        i += codepointSize;
    }

    // Re-allocate buffer to the actual number of codepoints loaded
    int *temp = (int *)RL_REALLOC(codepoints, codepointCount*sizeof(int));
    if (temp != NULL) codepoints = temp;

    if (count != NULL) *count = codepointCount;

    return codepoints;
}

// Unload codepoints data from memory
void UnloadCodepoints(int *codepoints)
{
    RL_FREE(codepoints);
}

// Get total number of codepoints in a UTF-8 encoded string
int GetCodepointCount(const char *text)
{
    if (text == NULL) return 0;

    int length = 0;
    const char *ptr = text;

    while (*ptr != '\0')
    {
        int next = 0;
        GetCodepointNext(ptr, &next);
        ptr += next;
        length++;
    }

    return length;
}

// Get next codepoint in a UTF-8 encoded string, 0x3f('?') is returned on failure
// NOTE: Kept for compatibility, it is an alias of GetCodepointNext()
int GetCodepoint(const char *text, int *codepointSize)
{
    return GetCodepointNext(text, codepointSize);
}

// Get next codepoint in a UTF-8 encoded string, 0x3f('?') is returned on failure
// NOTE: Sequences longer than 4 bytes, overlong encodings and truncated sequences fail
int GetCodepointNext(const char *text, int *codepointSize)
{
    int codepoint = 0x3f;       // Codepoint (defaults to '?')
    int size = 1;

    if (text != NULL)
    {
        const unsigned char *ptr = (const unsigned char *)text;

        if (0xf0 == (0xf8 & ptr[0]))
        {
            // 4 byte UTF-8 codepoint
            if (((ptr[1] & 0xc0) ^ 0x80) || ((ptr[2] & 0xc0) ^ 0x80) || ((ptr[3] & 0xc0) ^ 0x80)) { /* 10xxxxxx checks */ }
            else
            {
                codepoint = ((0x07 & ptr[0]) << 18) | ((0x3f & ptr[1]) << 12) | ((0x3f & ptr[2]) << 6) | (0x3f & ptr[3]);
                size = 4;
            }
        }
        else if (0xe0 == (0xf0 & ptr[0]))
        {
            // 3 byte UTF-8 codepoint
            if (((ptr[1] & 0xc0) ^ 0x80) || ((ptr[2] & 0xc0) ^ 0x80)) { /* 10xxxxxx checks */ }
            else
            {
                codepoint = ((0x0f & ptr[0]) << 12) | ((0x3f & ptr[1]) << 6) | (0x3f & ptr[2]);
                size = 3;
            }
        }
        else if (0xc0 == (0xe0 & ptr[0]))
        {
            // 2 byte UTF-8 codepoint
            if ((ptr[1] & 0xc0) ^ 0x80) { /* 10xxxxxx check */ }
            else
            {
                codepoint = ((0x1f & ptr[0]) << 6) | (0x3f & ptr[1]);
                size = 2;
            }
        }
        else if (0x00 == (0x80 & ptr[0]))
        {
            // 1 byte UTF-8 codepoint
            codepoint = ptr[0];
            size = 1;
        }
    }

    if (codepointSize != NULL) *codepointSize = size;

    return codepoint;
}

// Get previous codepoint in a UTF-8 encoded string, 0x3f('?') is returned on failure
// NOTE: text must point inside a valid UTF-8 buffer, at least one byte before it must be readable
int GetCodepointPrevious(const char *text, int *codepointSize)
{
    int codepoint = 0x3f;
    int size = 0;

    if (text != NULL)
    {
        const char *ptr = text;

        // Move to the start of the previous codepoint (skipping 10xxxxxx continuation bytes)
        do { ptr--; } while (((0x80 & ptr[0]) != 0) && ((0xc0 & ptr[0]) == 0x80));

        int cpSize = 0;
        codepoint = GetCodepointNext(ptr, &cpSize);
        size = cpSize;
    }

    if (codepointSize != NULL) *codepointSize = size;

    return codepoint;
}

// Encode one codepoint into UTF-8 byte array (array length returned as parameter)
// NOTE: Returned pointer refers to an internal static buffer
const char *CodepointToUTF8(int codepoint, int *utf8Size)
{
    static char utf8[6] = { 0 };
    memset(utf8, 0, 6);

    int size = 0;

    if (codepoint <= 0x7f)
    {
        utf8[0] = (char)codepoint;
        size = 1;
    }
    else if (codepoint <= 0x7ff)
    {
        utf8[0] = (char)(((codepoint >> 6) & 0x1f) | 0xc0);
        utf8[1] = (char)((codepoint & 0x3f) | 0x80);
        size = 2;
    }
    else if (codepoint <= 0xffff)
    {
        utf8[0] = (char)(((codepoint >> 12) & 0x0f) | 0xe0);
        utf8[1] = (char)(((codepoint >> 6) & 0x3f) | 0x80);
        utf8[2] = (char)((codepoint & 0x3f) | 0x80);
        size = 3;
    }
    else if (codepoint <= 0x10ffff)
    {
        utf8[0] = (char)(((codepoint >> 18) & 0x07) | 0xf0);
        utf8[1] = (char)(((codepoint >> 12) & 0x3f) | 0x80);
        utf8[2] = (char)(((codepoint >> 6) & 0x3f) | 0x80);
        utf8[3] = (char)((codepoint & 0x3f) | 0x80);
        size = 4;
    }
    else
    {
        // Out of Unicode range, encode the replacement '?' character
        utf8[0] = (char)0x3f;
        size = 1;
    }

    if (utf8Size != NULL) *utf8Size = size;

    return utf8;
}

//----------------------------------------------------------------------------------
// Text strings management (no UTF-8 strings, only byte chars)
// WARNING: Most of these functions use internal static buffers, store returned data
// on user-side for re-use. Functions suffixed *Alloc() return MemFree()-able memory.
//----------------------------------------------------------------------------------

// Load text as separate lines ('\n'), memory must be freed with UnloadTextLines()
char **LoadTextLines(const char *text, int *count)
{
    if (count != NULL) *count = 0;
    if ((text == NULL) || (text[0] == '\0')) return NULL;

    // Count lines: one more than the number of '\n' separators, ignoring a trailing one
    int lineCount = 1;
    for (int i = 0; text[i] != '\0'; i++)
    {
        if ((text[i] == '\n') && (text[i + 1] != '\0')) lineCount++;
    }

    char **lines = (char **)RL_CALLOC(lineCount, sizeof(char *));
    if (lines == NULL) return NULL;

    int index = 0;
    int lineStart = 0;
    int i = 0;

    for (; index < lineCount; i++)
    {
        if ((text[i] == '\n') || (text[i] == '\0'))
        {
            int lineLength = i - lineStart;
            if ((lineLength > 0) && (text[i - 1] == '\r')) lineLength--;    // Support CRLF line endings

            lines[index] = (char *)RL_CALLOC(lineLength + 1, 1);

            if (lines[index] != NULL)
            {
                memcpy(lines[index], text + lineStart, lineLength);
                lines[index][lineLength] = '\0';
            }

            index++;
            lineStart = i + 1;

            if (text[i] == '\0') break;
        }
    }

    if (count != NULL) *count = index;

    return lines;
}

// Unload text lines
void UnloadTextLines(char **text, int lineCount)
{
    if (text == NULL) return;

    for (int i = 0; i < lineCount; i++) RL_FREE(text[i]);

    RL_FREE(text);
}

// Copy one string to another, returns bytes copied
int TextCopy(char *dst, const char *src)
{
    int bytes = 0;

    if ((src != NULL) && (dst != NULL))
    {
        while (*src != '\0')
        {
            *dst = *src;
            dst++;
            src++;
            bytes++;
        }

        *dst = '\0';
    }

    return bytes;
}

// Check if two text strings are equal
bool TextIsEqual(const char *text1, const char *text2)
{
    if ((text1 == NULL) || (text2 == NULL)) return false;

    return (strcmp(text1, text2) == 0);
}

// Get text length, checks for '\0' ending
unsigned int TextLength(const char *text)
{
    if (text == NULL) return 0;

    return (unsigned int)strlen(text);
}

// Formatting of text with variables to 'embed'
// WARNING: String returned will expire after MAX_TEXTFORMAT_BUFFERS calls
const char *TextFormat(const char *text, ...)
{
    static char buffers[MAX_TEXTFORMAT_BUFFERS][MAX_TEXT_BUFFER_LENGTH] = { { 0 } };
    static int index = 0;

    char *currentBuffer = buffers[index];
    memset(currentBuffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if (text != NULL)
    {
        va_list args;
        va_start(args, text);
        int requiredByteCount = vsnprintf(currentBuffer, MAX_TEXT_BUFFER_LENGTH, text, args);
        va_end(args);

        // If requiredByteCount is bigger than the buffer, an overflow occurred: mark it as truncated
        if (requiredByteCount >= MAX_TEXT_BUFFER_LENGTH)
        {
            char *truncBuffer = currentBuffer + MAX_TEXT_BUFFER_LENGTH - 4;  // "...\0"
            truncBuffer[0] = '.';
            truncBuffer[1] = '.';
            truncBuffer[2] = '.';
            truncBuffer[3] = '\0';
        }
    }

    index += 1;     // Move to next buffer for next function call
    if (index >= MAX_TEXTFORMAT_BUFFERS) index = 0;

    return currentBuffer;
}

// Get a piece of a text string
const char *TextSubtext(const char *text, int position, int length)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if (text == NULL) return buffer;

    int textLength = (int)TextLength(text);

    if (position < 0) position = 0;
    if (length < 0) length = 0;
    if (position >= textLength) return buffer;
    if ((position + length) > textLength) length = textLength - position;
    if (length >= MAX_TEXT_BUFFER_LENGTH) length = MAX_TEXT_BUFFER_LENGTH - 1;

    memcpy(buffer, text + position, length);
    buffer[length] = '\0';

    return buffer;
}

// Remove all whitespace characters from text, concat words
const char *TextRemoveSpaces(const char *text)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if (text == NULL) return buffer;

    int index = 0;

    for (int i = 0; (text[i] != '\0') && (index < (MAX_TEXT_BUFFER_LENGTH - 1)); i++)
    {
        if ((text[i] != ' ') && (text[i] != '\t') && (text[i] != '\n') && (text[i] != '\r'))
        {
            buffer[index] = text[i];
            index++;
        }
    }

    buffer[index] = '\0';

    return buffer;
}

// Get text between two strings (delimiters not included)
// NOTE: Returns pointer to internal static buffer, NULL if delimiters are not found
char *GetTextBetween(const char *text, const char *begin, const char *end)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    int start = 0, length = 0;
    if (!TextGetBetweenRange(text, begin, end, &start, &length)) return NULL;

    if (length >= MAX_TEXT_BUFFER_LENGTH) length = MAX_TEXT_BUFFER_LENGTH - 1;

    memcpy(buffer, text + start, length);
    buffer[length] = '\0';

    return buffer;
}

// Replace text string with new string
// NOTE: Returns pointer to internal static buffer, result is truncated on overflow
char *TextReplace(const char *text, const char *search, const char *replacement)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    return TextReplaceInternal(text, search, replacement, buffer, MAX_TEXT_BUFFER_LENGTH);
}

// Replace text string with new string
// WARNING: Returned memory must be freed with MemFree()
char *TextReplaceAlloc(const char *text, const char *search, const char *replacement)
{
    if ((text == NULL) || (search == NULL) || (replacement == NULL) || (search[0] == '\0')) return NULL;

    int textLength = (int)TextLength(text);
    int searchLength = (int)TextLength(search);
    int replacementLength = (int)TextLength(replacement);

    // Count occurrences to size the output buffer exactly
    int count = 0;
    for (int i = 0; i <= (textLength - searchLength); )
    {
        if (memcmp(text + i, search, searchLength) == 0) { count++; i += searchLength; }
        else i++;
    }

    int resultSize = textLength + count*(replacementLength - searchLength) + 1;
    char *result = (char *)RL_CALLOC(resultSize, 1);
    if (result == NULL) return NULL;

    TextReplaceInternal(text, search, replacement, result, resultSize);

    return result;
}

// Replace text between two specific strings (delimiters are preserved)
// NOTE: Returns pointer to internal static buffer, original text is returned if not found
char *TextReplaceBetween(const char *text, const char *begin, const char *end, const char *replacement)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if (text == NULL) return buffer;

    int start = 0, length = 0;

    if ((replacement == NULL) || !TextGetBetweenRange(text, begin, end, &start, &length))
    {
        TextCopy(buffer, TextSubtext(text, 0, MAX_TEXT_BUFFER_LENGTH - 1));
        return buffer;
    }

    int textLength = (int)TextLength(text);
    int replacementLength = (int)TextLength(replacement);
    int index = 0;

    for (int i = 0; (i < start) && (index < (MAX_TEXT_BUFFER_LENGTH - 1)); i++) buffer[index++] = text[i];
    for (int i = 0; (i < replacementLength) && (index < (MAX_TEXT_BUFFER_LENGTH - 1)); i++) buffer[index++] = replacement[i];
    for (int i = start + length; (i < textLength) && (index < (MAX_TEXT_BUFFER_LENGTH - 1)); i++) buffer[index++] = text[i];

    buffer[index] = '\0';

    return buffer;
}

// Replace text between two specific strings (delimiters are preserved)
// WARNING: Returned memory must be freed with MemFree()
char *TextReplaceBetweenAlloc(const char *text, const char *begin, const char *end, const char *replacement)
{
    if ((text == NULL) || (replacement == NULL)) return NULL;

    int start = 0, length = 0;
    if (!TextGetBetweenRange(text, begin, end, &start, &length)) return NULL;

    int textLength = (int)TextLength(text);
    int replacementLength = (int)TextLength(replacement);

    char *result = (char *)RL_CALLOC(textLength - length + replacementLength + 1, 1);
    if (result == NULL) return NULL;

    memcpy(result, text, start);
    memcpy(result + start, replacement, replacementLength);
    memcpy(result + start + replacementLength, text + start + length, textLength - start - length);
    result[textLength - length + replacementLength] = '\0';

    return result;
}

// Insert text in a specific byte position
// NOTE: Returns pointer to internal static buffer, result is truncated on overflow
char *TextInsert(const char *text, const char *insert, int position)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    return TextInsertInternal(text, insert, position, buffer, MAX_TEXT_BUFFER_LENGTH);
}

// Insert text in a specific byte position
// WARNING: Returned memory must be freed with MemFree()
char *TextInsertAlloc(const char *text, const char *insert, int position)
{
    if ((text == NULL) || (insert == NULL)) return NULL;

    int textLength = (int)TextLength(text);
    int insertLength = (int)TextLength(insert);

    if ((position < 0) || (position > textLength)) return NULL;

    char *result = (char *)RL_CALLOC(textLength + insertLength + 1, 1);
    if (result == NULL) return NULL;

    TextInsertInternal(text, insert, position, result, textLength + insertLength + 1);

    return result;
}

// Join text strings with delimiter
// NOTE: Returns pointer to internal static buffer
char *TextJoin(char **textList, int count, const char *delimiter)
{
    static char text[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(text, 0, MAX_TEXT_BUFFER_LENGTH);

    if ((textList == NULL) || (count <= 0)) return text;

    int delimiterLength = (int)TextLength(delimiter);
    int totalLength = 0;

    for (int i = 0; i < count; i++)
    {
        int textLength = (int)TextLength(textList[i]);

        // Make sure joined text can fit inside MAX_TEXT_BUFFER_LENGTH
        if ((totalLength + textLength) >= MAX_TEXT_BUFFER_LENGTH) break;

        if (textLength > 0)
        {
            memcpy(text + totalLength, textList[i], textLength);
            totalLength += textLength;
        }

        if ((delimiterLength > 0) && (i < (count - 1)))
        {
            if ((totalLength + delimiterLength) >= MAX_TEXT_BUFFER_LENGTH) break;

            memcpy(text + totalLength, delimiter, delimiterLength);
            totalLength += delimiterLength;
        }
    }

    text[totalLength] = '\0';

    return text;
}

// Split string into multiple strings
// NOTE: Returns pointer to internal static buffers, up to MAX_TEXTSPLIT_COUNT substrings
char **TextSplit(const char *text, char delimiter, int *count)
{
    static char *result[MAX_TEXTSPLIT_COUNT] = { NULL };
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };

    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);
    for (int i = 0; i < MAX_TEXTSPLIT_COUNT; i++) result[i] = NULL;
    result[0] = buffer;

    int counter = 0;

    if (text != NULL)
    {
        counter = 1;

        // Count how many substrings we have on text and point to every one
        for (int i = 0; i < (MAX_TEXT_BUFFER_LENGTH - 1); i++)
        {
            buffer[i] = text[i];
            if (buffer[i] == '\0') break;
            else if (buffer[i] == delimiter)
            {
                buffer[i] = '\0';       // Set an end of string at this point
                result[counter] = buffer + i + 1;
                counter++;

                if (counter == MAX_TEXTSPLIT_COUNT) break;
            }
        }
    }

    if (count != NULL) *count = counter;

    return result;
}

// Append text at specific position and move cursor
// NOTE: dst buffer must be big enough to fit the appended text
void TextAppend(char *text, const char *append, int *position)
{
    if ((text == NULL) || (append == NULL) || (position == NULL)) return;

    int appendLength = (int)TextLength(append);

    memcpy(text + *position, append, appendLength);
    text[*position + appendLength] = '\0';

    *position += appendLength;
}

// Find first text occurrence within a string, -1 if not found
int TextFindIndex(const char *text, const char *search)
{
    if ((text == NULL) || (search == NULL)) return -1;

    const char *ptr = strstr(text, search);
    if (ptr == NULL) return -1;

    return (int)(ptr - text);
}

// Get upper case version of provided string
char *TextToUpper(const char *text)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if (text == NULL) return buffer;

    int i = 0;
    for (; (i < (MAX_TEXT_BUFFER_LENGTH - 1)) && (text[i] != '\0'); i++)
    {
        if ((text[i] >= 'a') && (text[i] <= 'z')) buffer[i] = text[i] - 32;
        else buffer[i] = text[i];
    }
    buffer[i] = '\0';

    return buffer;
}

// Get lower case version of provided string
char *TextToLower(const char *text)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if (text == NULL) return buffer;

    int i = 0;
    for (; (i < (MAX_TEXT_BUFFER_LENGTH - 1)) && (text[i] != '\0'); i++)
    {
        if ((text[i] >= 'A') && (text[i] <= 'Z')) buffer[i] = text[i] + 32;
        else buffer[i] = text[i];
    }
    buffer[i] = '\0';

    return buffer;
}

// Get Pascal case notation version of provided string ("some_text" -> "SomeText")
char *TextToPascal(const char *text)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if ((text == NULL) || (text[0] == '\0')) return buffer;

    // Upper case first character
    if ((text[0] >= 'a') && (text[0] <= 'z')) buffer[0] = text[0] - 32;
    else buffer[0] = text[0];

    // Check for the separator to upper case the next character
    int i = 1;
    int j = 1;

    for (; (i < (MAX_TEXT_BUFFER_LENGTH - 1)) && (text[j] != '\0'); i++, j++)
    {
        if ((text[j] != '_') && (text[j] != '-') && (text[j] != ' ')) buffer[i] = text[j];
        else
        {
            j++;
            if (text[j] == '\0') break;

            if ((text[j] >= 'a') && (text[j] <= 'z')) buffer[i] = text[j] - 32;
            else buffer[i] = text[j];
        }
    }

    buffer[i] = '\0';

    return buffer;
}

// Get Snake case notation version of provided string ("SomeText" -> "some_text")
char *TextToSnake(const char *text)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if (text == NULL) return buffer;

    int i = 0;

    for (int j = 0; (i < (MAX_TEXT_BUFFER_LENGTH - 2)) && (text[j] != '\0'); j++)
    {
        if ((text[j] >= 'A') && (text[j] <= 'Z'))
        {
            if (i >= 1) buffer[i++] = '_';
            buffer[i++] = text[j] + 32;
        }
        else if ((text[j] == '-') || (text[j] == ' ')) buffer[i++] = '_';
        else buffer[i++] = text[j];
    }

    buffer[i] = '\0';

    return buffer;
}

// Get Camel case notation version of provided string ("some_text" -> "someText")
char *TextToCamel(const char *text)
{
    static char buffer[MAX_TEXT_BUFFER_LENGTH] = { 0 };
    memset(buffer, 0, MAX_TEXT_BUFFER_LENGTH);

    if ((text == NULL) || (text[0] == '\0')) return buffer;

    TextCopy(buffer, TextToPascal(text));

    // Lower case first character
    if ((buffer[0] >= 'A') && (buffer[0] <= 'Z')) buffer[0] = buffer[0] + 32;

    return buffer;
}

// Get integer value from text (negative values supported)
int TextToInteger(const char *text)
{
    if (text == NULL) return 0;

    int value = 0;
    int sign = 1;

    if ((text[0] == '+') || (text[0] == '-'))
    {
        if (text[0] == '-') sign = -1;
        text++;
    }

    for (int i = 0; ((text[i] >= '0') && (text[i] <= '9')); i++) value = value*10 + (int)(text[i] - '0');

    return value*sign;
}

// Get float value from text (negative values supported)
float TextToFloat(const char *text)
{
    if (text == NULL) return 0.0f;

    float value = 0.0f;
    float sign = 1.0f;

    if ((text[0] == '+') || (text[0] == '-'))
    {
        if (text[0] == '-') sign = -1.0f;
        text++;
    }

    int i = 0;
    for (; ((text[i] >= '0') && (text[i] <= '9')); i++) value = value*10.0f + (float)(text[i] - '0');

    if (text[i] != '.') return value*sign;
    i++;

    float divisor = 10.0f;
    for (; ((text[i] >= '0') && (text[i] <= '9')); i++)
    {
        value += ((float)(text[i] - '0'))/divisor;
        divisor = divisor*10.0f;
    }

    return value*sign;
}

//----------------------------------------------------------------------------------
// Module internal functions definition
//----------------------------------------------------------------------------------

// Emit one textured quad into the rlvk batch (rlgl style, no rtextures dependency)
// NOTE: src is in texture pixels, dst in screen units, current modelview matrix applies
static void DrawGlyphQuad(Texture2D texture, Rectangle src, Rectangle dst, Color tint)
{
    if ((texture.id == 0) || (texture.width <= 0) || (texture.height <= 0)) return;

    float width = (float)texture.width;
    float height = (float)texture.height;

    bool flipX = false;
    if (src.width < 0.0f) { flipX = true; src.width *= -1.0f; }
    if (src.height < 0.0f) src.y -= src.height;

    rlCheckRenderBatchLimit(4);

    rlSetTexture(texture.id);

    rlBegin(RL_QUADS);

        rlColor4ub(tint.r, tint.g, tint.b, tint.a);
        rlNormal3f(0.0f, 0.0f, 1.0f);       // Normal vector pointing towards viewer

        // Top-left corner for texture and quad
        rlTexCoord2f(flipX? (src.x + src.width)/width : src.x/width, src.y/height);
        rlVertex2f(dst.x, dst.y);

        // Bottom-left corner for texture and quad
        rlTexCoord2f(flipX? (src.x + src.width)/width : src.x/width, (src.y + src.height)/height);
        rlVertex2f(dst.x, dst.y + dst.height);

        // Bottom-right corner for texture and quad
        rlTexCoord2f(flipX? src.x/width : (src.x + src.width)/width, (src.y + src.height)/height);
        rlVertex2f(dst.x + dst.width, dst.y + dst.height);

        // Top-right corner for texture and quad
        rlTexCoord2f(flipX? src.x/width : (src.x + src.width)/width, src.y/height);
        rlVertex2f(dst.x + dst.width, dst.y);

    rlEnd();

    rlSetTexture(0);
}

// Measure text size, shared by MeasureTextEx() and MeasureTextCodepoints()
// NOTE: Exactly one of text/codepoints must be provided (text is UTF-8, codepoints is an array)
static Vector2 MeasureTextInternal(Font font, const char *text, const int *codepoints, int length, float fontSize, float spacing)
{
    Vector2 textSize = { 0.0f, 0.0f };

    if (!IsFontValid(font)) return textSize;

    int size = (text != NULL)? (int)TextLength(text) : length;
    if (size <= 0) return textSize;

    float tempByteCounter = 0.0f;       // Used to count longer text line num chars
    float byteCounter = 0.0f;

    float textWidth = 0.0f;
    float tempTextWidth = 0.0f;         // Used to count longer text line width
    float textHeight = fontSize;
    float scaleFactor = fontSize/(float)font.baseSize;

    for (int i = 0; i < size;)
    {
        byteCounter += 1.0f;

        int letter = 0;

        if (text != NULL)
        {
            int codepointByteCount = 0;
            letter = GetCodepointNext(&text[i], &codepointByteCount);
            i += codepointByteCount;
        }
        else
        {
            letter = codepoints[i];
            i++;
        }

        int index = GetGlyphIndex(font, letter);

        if (letter != '\n')
        {
            if (font.glyphs[index].advanceX > 0) textWidth += font.glyphs[index].advanceX;
            else textWidth += (font.recs[index].width + font.glyphs[index].offsetX);
        }
        else
        {
            if (tempTextWidth < textWidth) tempTextWidth = textWidth;
            byteCounter = 0.0f;
            textWidth = 0.0f;

            // NOTE: Line spacing is a global variable, use SetTextLineSpacing() to setup
            textHeight += (fontSize + textLineSpacing);
        }

        if (tempByteCounter < byteCounter) tempByteCounter = byteCounter;
    }

    if (tempTextWidth < textWidth) tempTextWidth = textWidth;

    textSize.x = tempTextWidth*scaleFactor + (float)((tempByteCounter - 1.0f)*spacing);
    textSize.y = textHeight;

    return textSize;
}

// Replace every occurrence of search by replacement into dst (always '\0' terminated)
// NOTE: dstSize > 0, output is truncated if it does not fit
static char *TextReplaceInternal(const char *text, const char *search, const char *replacement, char *dst, int dstSize)
{
    if (dst == NULL) return NULL;

    dst[0] = '\0';

    if ((text == NULL) || (search == NULL) || (replacement == NULL) || (dstSize <= 1)) return dst;

    int searchLength = (int)TextLength(search);
    int replacementLength = (int)TextLength(replacement);
    int textLength = (int)TextLength(text);

    if (searchLength == 0)
    {
        int copyLength = (textLength < (dstSize - 1))? textLength : (dstSize - 1);
        memcpy(dst, text, copyLength);
        dst[copyLength] = '\0';
        return dst;
    }

    int index = 0;

    for (int i = 0; (i < textLength) && (index < (dstSize - 1)); )
    {
        if (((textLength - i) >= searchLength) && (memcmp(text + i, search, searchLength) == 0))
        {
            for (int j = 0; (j < replacementLength) && (index < (dstSize - 1)); j++) dst[index++] = replacement[j];
            i += searchLength;
        }
        else dst[index++] = text[i++];
    }

    dst[index] = '\0';

    return dst;
}

// Insert 'insert' into 'text' at byte 'position', writing into dst (always '\0' terminated)
// NOTE: dstSize > 0, output is truncated if it does not fit, invalid positions return an empty string
static char *TextInsertInternal(const char *text, const char *insert, int position, char *dst, int dstSize)
{
    if (dst == NULL) return NULL;

    dst[0] = '\0';

    if ((text == NULL) || (insert == NULL) || (dstSize <= 1)) return dst;

    int textLength = (int)TextLength(text);
    int insertLength = (int)TextLength(insert);

    if ((position < 0) || (position > textLength))
    {
        TRACELOG(LOG_WARNING, "TEXT: Insert position out of bounds (%i), returning empty text", position);
        return dst;
    }

    int index = 0;

    for (int i = 0; (i < position) && (index < (dstSize - 1)); i++) dst[index++] = text[i];
    for (int i = 0; (i < insertLength) && (index < (dstSize - 1)); i++) dst[index++] = insert[i];
    for (int i = position; (i < textLength) && (index < (dstSize - 1)); i++) dst[index++] = text[i];

    dst[index] = '\0';

    return dst;
}

// Get the byte range located between the first 'begin' and the following 'end' delimiters
// NOTE: start/length nonnull, returns false if any delimiter is not found
static bool TextGetBetweenRange(const char *text, const char *begin, const char *end, int *start, int *length)
{
    *start = 0;
    *length = 0;

    if ((text == NULL) || (begin == NULL) || (end == NULL)) return false;

    const char *beginPtr = strstr(text, begin);
    if (beginPtr == NULL) return false;

    const char *contentPtr = beginPtr + TextLength(begin);

    const char *endPtr = strstr(contentPtr, end);
    if (endPtr == NULL) return false;

    *start = (int)(contentPtr - text);
    *length = (int)(endPtr - contentPtr);

    return true;
}

#if (defined(SUPPORT_FILEFORMAT_FNT) && SUPPORT_FILEFORMAT_FNT) || (defined(SUPPORT_FILEFORMAT_BDF) && SUPPORT_FILEFORMAT_BDF)
// Read one text line into buffer, returns the number of bytes read (line separator excluded)
// NOTE: buffer nonnull, maxLength > 0, buffer is always '\0' terminated
static int GetLine(const char *origin, char *buffer, int maxLength)
{
    int count = 0;

    for (; count < (maxLength - 1); count++)
    {
        if ((origin[count] == '\n') || (origin[count] == '\0')) break;
    }

    memcpy(buffer, origin, count);
    buffer[count] = '\0';

    // Strip trailing carriage return, so CRLF files parse identically to LF files
    if ((count > 0) && (buffer[count - 1] == '\r')) buffer[count - 1] = '\0';

    return count;
}
#endif

#if defined(SUPPORT_FILEFORMAT_FNT) && SUPPORT_FILEFORMAT_FNT
// Load a BMFont file (AngelCode font file), only single page fonts supported
static Font LoadBMFont(const char *fileName)
{
    #define MAX_BUFFER_SIZE     256

    Font font = { 0 };

    char buffer[MAX_BUFFER_SIZE] = { 0 };
    char imFileName[129] = { 0 };
    int fontSize = 0, base = 0, imWidth = 0, imHeight = 0, pageCount = 1, glyphCount = 0;

    char *fileText = LoadFileText(fileName);
    if (fileText == NULL) return font;

    char *fileTextPtr = fileText;

    // NOTE: First line ("info ...") contains no data required here
    int lineBytes = GetLine(fileTextPtr, buffer, MAX_BUFFER_SIZE);
    fileTextPtr += (lineBytes + 1);

    // Line: "common lineHeight=... base=... scaleW=... scaleH=... pages=..."
    lineBytes = GetLine(fileTextPtr, buffer, MAX_BUFFER_SIZE);
    char *searchPoint = strstr(buffer, "lineHeight");
    if (searchPoint != NULL) sscanf(searchPoint, "lineHeight=%i base=%i scaleW=%i scaleH=%i pages=%i", &fontSize, &base, &imWidth, &imHeight, &pageCount);
    fileTextPtr += (lineBytes + 1);

    if (pageCount > 1) TRACELOG(LOG_WARNING, "FONT: [%s] Multiple pages BMFont not supported, only page 0 is loaded", fileName);

    // Line: "page id=0 file=\"...\""
    lineBytes = GetLine(fileTextPtr, buffer, MAX_BUFFER_SIZE);
    searchPoint = strstr(buffer, "file");
    if (searchPoint != NULL) sscanf(searchPoint, "file=\"%128[^\"]\"", imFileName);
    fileTextPtr += (lineBytes + 1);

    // Line: "chars count=..."
    lineBytes = GetLine(fileTextPtr, buffer, MAX_BUFFER_SIZE);
    searchPoint = strstr(buffer, "count");
    if (searchPoint != NULL) sscanf(searchPoint, "count=%i", &glyphCount);
    fileTextPtr += (lineBytes + 1);

    if ((fontSize <= 0) || (glyphCount <= 0) || (imFileName[0] == '\0'))
    {
        TRACELOG(LOG_WARNING, "FONT: [%s] Failed to parse BMFont header data", fileName);
        UnloadFileText(fileText);
        return font;
    }

    // Compose the atlas image path from the .fnt directory and the referenced file name
    char *imPath = NULL;
    const char *lastSlash = strrchr(fileName, '/');
    if (lastSlash == NULL) lastSlash = strrchr(fileName, '\\');

    if (lastSlash != NULL)
    {
        int dirLength = (int)(lastSlash - fileName) + 1;
        imPath = (char *)RL_CALLOC(dirLength + (int)TextLength(imFileName) + 1, 1);

        if (imPath != NULL)
        {
            memcpy(imPath, fileName, dirLength);
            memcpy(imPath + dirLength, imFileName, TextLength(imFileName));
        }
    }

    Image imFont = LoadImage((imPath != NULL)? imPath : imFileName);
    RL_FREE(imPath);

    if (imFont.data == NULL)
    {
        TRACELOG(LOG_WARNING, "FONT: [%s] Failed to load BMFont atlas image: %s", fileName, imFileName);
        UnloadFileText(fileText);
        return font;
    }

    if (imFont.format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE)
    {
        // Convert image to GRAYSCALE + ALPHA, using the mask as the alpha channel
        unsigned char *dataGrayAlpha = (unsigned char *)RL_CALLOC(imFont.width*imFont.height, 2);

        if (dataGrayAlpha != NULL)
        {
            for (int p = 0, i = 0; p < (imFont.width*imFont.height*2); p += 2, i++)
            {
                dataGrayAlpha[p] = 0xff;
                dataGrayAlpha[p + 1] = ((unsigned char *)imFont.data)[i];
            }

            Image imFontAlpha = {
                .data = dataGrayAlpha,
                .width = imFont.width,
                .height = imFont.height,
                .mipmaps = 1,
                .format = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA
            };

            UnloadImage(imFont);
            imFont = imFontAlpha;
        }
    }

    font.texture = LoadTextureFromImage(imFont);
    font.baseSize = fontSize;
    font.glyphCount = glyphCount;
    font.glyphPadding = 0;
    font.glyphs = (GlyphInfo *)RL_CALLOC(glyphCount, sizeof(GlyphInfo));
    font.recs = (Rectangle *)RL_CALLOC(glyphCount, sizeof(Rectangle));

    if ((font.glyphs == NULL) || (font.recs == NULL))
    {
        RL_FREE(font.glyphs);
        RL_FREE(font.recs);
        if (font.texture.id != 0) UnloadTexture(font.texture);
        UnloadImage(imFont);
        UnloadFileText(fileText);
        return (Font){ 0 };
    }

    for (int i = 0; i < glyphCount; i++)
    {
        int charId = 0, charX = 0, charY = 0, charWidth = 0, charHeight = 0;
        int charOffsetX = 0, charOffsetY = 0, charAdvanceX = 0, pageID = 0;

        lineBytes = GetLine(fileTextPtr, buffer, MAX_BUFFER_SIZE);
        fileTextPtr += (lineBytes + 1);

        if (sscanf(buffer, "char id=%i x=%i y=%i width=%i height=%i xoffset=%i yoffset=%i xadvance=%i page=%i",
                   &charId, &charX, &charY, &charWidth, &charHeight, &charOffsetX, &charOffsetY, &charAdvanceX, &pageID) < 8)
        {
            TRACELOG(LOG_WARNING, "FONT: [%s] Failed to parse BMFont glyph line (%i)", fileName, i);
            continue;
        }

        if (pageID != 0) continue;      // Multiple pages not supported

        font.recs[i] = (Rectangle){ (float)charX, (float)charY, (float)charWidth, (float)charHeight };
        font.glyphs[i].value = charId;
        font.glyphs[i].offsetX = charOffsetX;
        font.glyphs[i].offsetY = charOffsetY;
        font.glyphs[i].advanceX = charAdvanceX;
        font.glyphs[i].image = ImageFromImage(imFont, font.recs[i]);
    }

    UnloadImage(imFont);
    UnloadFileText(fileText);

    if (font.texture.id == 0) TRACELOG(LOG_WARNING, "FONT: [%s] Failed to load BMFont texture", fileName);
    else TRACELOG(LOG_INFO, "FONT: [%s] BMFont loaded successfully (%i glyphs)", fileName, font.glyphCount);

    return font;
}
#endif  // SUPPORT_FILEFORMAT_FNT

#if defined(SUPPORT_FILEFORMAT_BDF) && SUPPORT_FILEFORMAT_BDF
// Convert one hexadecimal ASCII digit to its value, -1 if not a valid digit
static int HexDigitValue(char c)
{
    if ((c >= '0') && (c <= '9')) return (c - '0');
    if ((c >= 'a') && (c <= 'f')) return (c - 'a' + 10);
    if ((c >= 'A') && (c <= 'F')) return (c - 'A' + 10);

    return -1;
}

// Load a BDF font file (Glyph Bitmap Distribution Format), monochrome bitmap glyphs
static Font LoadBDFFont(const char *fileName)
{
    #define MAX_BDF_LINE_SIZE   512

    Font font = { 0 };

    char *fileText = LoadFileText(fileName);
    if (fileText == NULL) return font;

    char buffer[MAX_BDF_LINE_SIZE] = { 0 };
    char *ptr = fileText;

    int fbbWidth = 0, fbbHeight = 0, fbbOffsetX = 0, fbbOffsetY = 0;
    int glyphCount = 0;

    // First pass over the header: font bounding box and glyph count
    while (*ptr != '\0')
    {
        int lineBytes = GetLine(ptr, buffer, MAX_BDF_LINE_SIZE);
        ptr += (lineBytes + 1);

        if (strstr(buffer, "FONTBOUNDINGBOX") == buffer) sscanf(buffer, "FONTBOUNDINGBOX %i %i %i %i", &fbbWidth, &fbbHeight, &fbbOffsetX, &fbbOffsetY);
        else if (strstr(buffer, "CHARS ") == buffer) { sscanf(buffer, "CHARS %i", &glyphCount); break; }

        if (buffer[0] == '\0' && *ptr == '\0') break;
    }

    if ((fbbHeight <= 0) || (glyphCount <= 0))
    {
        TRACELOG(LOG_WARNING, "FONT: [%s] Failed to parse BDF font header data", fileName);
        UnloadFileText(fileText);
        return font;
    }

    GlyphInfo *glyphs = (GlyphInfo *)RL_CALLOC(glyphCount, sizeof(GlyphInfo));

    if (glyphs == NULL)
    {
        UnloadFileText(fileText);
        return font;
    }

    int ascent = fbbHeight + fbbOffsetY;    // Distance from the baseline to the top of the bounding box
    int index = 0;

    while ((*ptr != '\0') && (index < glyphCount))
    {
        int lineBytes = GetLine(ptr, buffer, MAX_BDF_LINE_SIZE);
        ptr += (lineBytes + 1);

        if (strstr(buffer, "STARTCHAR") != buffer) continue;

        int encoding = -1, advanceX = 0, dummy = 0;
        int bbWidth = 0, bbHeight = 0, bbOffsetX = 0, bbOffsetY = 0;
        bool bitmapFound = false;

        // Parse glyph properties until the BITMAP section starts
        while (*ptr != '\0')
        {
            lineBytes = GetLine(ptr, buffer, MAX_BDF_LINE_SIZE);
            ptr += (lineBytes + 1);

            if (strstr(buffer, "ENCODING") == buffer) sscanf(buffer, "ENCODING %i", &encoding);
            else if (strstr(buffer, "DWIDTH") == buffer) sscanf(buffer, "DWIDTH %i %i", &advanceX, &dummy);
            else if (strstr(buffer, "BBX") == buffer) sscanf(buffer, "BBX %i %i %i %i", &bbWidth, &bbHeight, &bbOffsetX, &bbOffsetY);
            else if (strstr(buffer, "BITMAP") == buffer) { bitmapFound = true; break; }
            else if (strstr(buffer, "ENDCHAR") == buffer) break;
        }

        if (!bitmapFound || (encoding < 0)) continue;

        glyphs[index].value = encoding;
        glyphs[index].offsetX = bbOffsetX;
        glyphs[index].offsetY = ascent - (bbOffsetY + bbHeight);
        glyphs[index].advanceX = advanceX;
        glyphs[index].image.width = (bbWidth > 0)? bbWidth : 0;
        glyphs[index].image.height = (bbHeight > 0)? bbHeight : 0;
        glyphs[index].image.mipmaps = 1;
        glyphs[index].image.format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;

        if ((bbWidth > 0) && (bbHeight > 0))
        {
            unsigned char *pixels = (unsigned char *)RL_CALLOC(bbWidth*bbHeight, 1);
            glyphs[index].image.data = pixels;

            for (int y = 0; y < bbHeight; y++)
            {
                lineBytes = GetLine(ptr, buffer, MAX_BDF_LINE_SIZE);
                ptr += (lineBytes + 1);

                if (pixels == NULL) continue;

                for (int x = 0; x < bbWidth; x++)
                {
                    int digit = HexDigitValue(buffer[x/4]);
                    if (digit < 0) break;

                    if ((digit & (0x08 >> (x%4))) != 0) pixels[y*bbWidth + x] = 255;
                }
            }
        }

        index++;

        // Skip until ENDCHAR
        while (*ptr != '\0')
        {
            lineBytes = GetLine(ptr, buffer, MAX_BDF_LINE_SIZE);
            ptr += (lineBytes + 1);
            if (strstr(buffer, "ENDCHAR") == buffer) break;
        }
    }

    UnloadFileText(fileText);

    if (index == 0)
    {
        RL_FREE(glyphs);
        TRACELOG(LOG_WARNING, "FONT: [%s] No glyphs found on BDF font file", fileName);
        return font;
    }

    font.glyphs = glyphs;
    font.glyphCount = index;
    font.glyphPadding = 0;
    font.baseSize = fbbHeight;

    Image atlas = GenImageFontAtlas(font.glyphs, &font.recs, font.glyphCount, font.baseSize, font.glyphPadding, 0);
    font.texture = LoadTextureFromImage(atlas);

    for (int i = 0; i < font.glyphCount; i++)
    {
        UnloadImage(font.glyphs[i].image);
        font.glyphs[i].image = ImageFromImage(atlas, font.recs[i]);
    }

    UnloadImage(atlas);

    TRACELOG(LOG_INFO, "FONT: [%s] BDF font loaded successfully (%i glyphs)", fileName, font.glyphCount);

    return font;
}
#endif  // SUPPORT_FILEFORMAT_BDF
