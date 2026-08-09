/**********************************************************************************************
*
*   rcore - Platform-independent core module: window/input/timing/drawing-modes
*
*   This is the platform-independent half of the raylib core module, reimplemented on
*   top of the Vulkan renderer abstraction (rlvk.h). Everything that needs to talk to
*   the OS lives behind the Platform* contract declared in rcore_internal.h and is
*   implemented by exactly one rcore_<platform>.c backend.
*
*   MODULE OWNERSHIP (see CONVENTIONS.md)
*     This translation unit owns the implementations of: sdefl, sinfl, rprand,
*     rcamera and rgestures. It also provides the external (out-of-line) definitions
*     of raymath through RAYMATH_IMPLEMENTATION.
*
*   CONTRACT NOTES / DEVIATIONS FROM raylib (documented, no stubs)
*     - Shaders are SPIR-V: LoadShader() feeds file contents straight into
*       rlLoadShaderCodeSize(); GLSL source is NOT compiled at runtime.
*     - GetKeyName() is resolved from a built-in US-layout table because the platform
*       contract exposes no per-backend key-name query.
*     - Gesture events are sampled once per frame in PollInputEvents() (raylib feeds
*       them from the platform event callbacks), see UpdateGestureEvents().
*     - Vr stereo mode has no renderer support in rlvk (no stereo pipeline); it falls
*       back to rendering the left eye and warns once, instead of failing silently.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

// POSIX feature level required for: readlink(), mkdir(), getcwd(), opendir()...
// NOTE: Must be defined before any system header is included
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
    #define _POSIX_C_SOURCE 200809L
#endif

#include "rcore_internal.h"     // CoreData, Platform* contract (includes raylib.h + rlvk.h)

#undef TRACELOG                 // rcore_internal.h provides a fallback definition
#include "config.h"             // Defines TRACELOG() honoring SUPPORT_TRACELOG

#define RAYMATH_IMPLEMENTATION  // Provide the external definitions of raymath inline functions
#include "raymath.h"            // Vector2/Vector3/Matrix functionality

#include <stdlib.h>             // Required for: srand(), rand(), atexit(), malloc(), free()
#include <stdio.h>              // Required for: sprintf(), vsnprintf(), FILE
#include <string.h>             // Required for: strrchr(), strcmp(), strlen(), memset()
#include <time.h>               // Required for: time(), localtime()
#include <math.h>               // Required for: tan(), sqrtf()
#include <ctype.h>              // Required for: tolower()
#include <errno.h>              // Required for: errno
#include <sys/stat.h>           // Required for: stat(), S_ISREG, S_ISDIR, mkdir()

#if defined(_WIN32)
    #include <direct.h>         // Required for: _chdir(), _getcwd(), _mkdir()
    #define GETCWD _getcwd
    #define CHDIR _chdir
    #include "external/dirent.h"    // Required for: DIR, opendir(), closedir() [Windows only]
#else
    #include <unistd.h>         // Required for: getcwd(), chdir(), readlink()
    #include <dirent.h>         // Required for: DIR, opendir(), closedir()
    #define GETCWD getcwd
    #define CHDIR chdir
#endif

#if defined(__APPLE__)
    #include <mach-o/dyld.h>    // Required for: _NSGetExecutablePath()
#endif

#define SDEFL_IMPLEMENTATION
#include "external/sdefl.h"     // Deflate compression (CompressData)

#define SINFL_IMPLEMENTATION
#include "external/sinfl.h"     // Deflate decompression (DecompressData)

#if SUPPORT_RPRAND_GENERATOR
    #define RPRAND_IMPLEMENTATION
    #include "external/rprand.h"
#endif

#if SUPPORT_GESTURES_SYSTEM
    #define RGESTURES_IMPLEMENTATION
    #include "rgestures.h"      // Gestures detection functionality
#endif

#if SUPPORT_CAMERA_SYSTEM
    #define RCAMERA_IMPLEMENTATION
    #include "rcamera.h"        // Camera system functionality
#endif

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

#ifndef RL_MAX_SHADER_LOCATIONS
    #define RL_MAX_SHADER_LOCATIONS         32      // Shader.locs array size (raylib convention)
#endif
#ifndef MAX_FILEPATH_CAPACITY
    #define MAX_FILEPATH_CAPACITY         8192      // Maximum file paths capacity
#endif
#ifndef MAX_DECOMPRESSION_SIZE
    #define MAX_DECOMPRESSION_SIZE          64      // Max size allocated for decompression in MB
#endif
#ifndef MAX_AUTOMATION_EVENTS
    #define MAX_AUTOMATION_EVENTS        16384      // Maximum number of automation events to record
#endif
#ifndef MAX_GAMEPAD_VIBRATION_TIME
    #define MAX_GAMEPAD_VIBRATION_TIME    2.0f      // Maximum vibration time in seconds
#endif

// Default shader vertex attribute/uniform names (mirrors rlvk shader ABI)
#define RL_DEFAULT_SHADER_ATTRIB_NAME_POSITION      "vertexPosition"
#define RL_DEFAULT_SHADER_ATTRIB_NAME_TEXCOORD      "vertexTexCoord"
#define RL_DEFAULT_SHADER_ATTRIB_NAME_NORMAL        "vertexNormal"
#define RL_DEFAULT_SHADER_ATTRIB_NAME_COLOR         "vertexColor"
#define RL_DEFAULT_SHADER_ATTRIB_NAME_TANGENT       "vertexTangent"
#define RL_DEFAULT_SHADER_ATTRIB_NAME_TEXCOORD2     "vertexTexCoord2"
#define RL_DEFAULT_SHADER_ATTRIB_NAME_BONEIDS       "vertexBoneIds"
#define RL_DEFAULT_SHADER_ATTRIB_NAME_BONEWEIGHTS   "vertexBoneWeights"
#define RL_DEFAULT_SHADER_UNIFORM_NAME_MVP          "mvp"
#define RL_DEFAULT_SHADER_UNIFORM_NAME_VIEW         "matView"
#define RL_DEFAULT_SHADER_UNIFORM_NAME_PROJECTION   "matProjection"
#define RL_DEFAULT_SHADER_UNIFORM_NAME_MODEL        "matModel"
#define RL_DEFAULT_SHADER_UNIFORM_NAME_NORMAL       "matNormal"
#define RL_DEFAULT_SHADER_UNIFORM_NAME_COLOR        "colDiffuse"
#define RL_DEFAULT_SHADER_UNIFORM_NAME_BONEMATRICES "boneMatrices"
#define RL_DEFAULT_SHADER_SAMPLER2D_NAME_TEXTURE0   "texture0"
#define RL_DEFAULT_SHADER_SAMPLER2D_NAME_TEXTURE1   "texture1"
#define RL_DEFAULT_SHADER_SAMPLER2D_NAME_TEXTURE2   "texture2"

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------

// Automation event types, recorded/replayed by the automation events system
typedef enum AutomationEventType {
    EVENT_NONE = 0,
    // Input events
    INPUT_KEY_UP,                   // param[0]: key
    INPUT_KEY_DOWN,                 // param[0]: key
    INPUT_KEY_PRESSED,              // param[0]: key
    INPUT_KEY_RELEASED,             // param[0]: key
    INPUT_MOUSE_BUTTON_UP,          // param[0]: button
    INPUT_MOUSE_BUTTON_DOWN,        // param[0]: button
    INPUT_MOUSE_POSITION,           // param[0]: x, param[1]: y
    INPUT_MOUSE_WHEEL_MOTION,       // param[0]: x delta, param[1]: y delta
    INPUT_GAMEPAD_CONNECT,          // param[0]: gamepad
    INPUT_GAMEPAD_DISCONNECT,       // param[0]: gamepad
    INPUT_GAMEPAD_BUTTON_UP,        // param[0]: button
    INPUT_GAMEPAD_BUTTON_DOWN,      // param[0]: button
    INPUT_GAMEPAD_AXIS_MOTION,      // param[0]: axis, param[1]: delta
    INPUT_TOUCH_UP,                 // param[0]: id
    INPUT_TOUCH_DOWN,               // param[0]: id
    INPUT_TOUCH_POSITION,           // param[0]: x, param[1]: y
    INPUT_GESTURE,                  // param[0]: gesture
    // Window events
    WINDOW_CLOSE,                   // no params
    WINDOW_MAXIMIZE,                // no params
    WINDOW_MINIMIZE,                // no params
    WINDOW_RESIZE,                  // param[0]: width, param[1]: height
    // Custom events
    ACTION_TAKE_SCREENSHOT,         // no params
    ACTION_SETTARGETFPS             // param[0]: fps
} AutomationEventType;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
CoreData CORE = { 0 };              // Global CORE state context

#if SUPPORT_SCREEN_CAPTURE
static int screenshotCounter = 0;   // Screenshots counter
#endif

#if SUPPORT_AUTOMATION_EVENTS
static AutomationEventList *currentEventList = NULL;    // Current automation events list, set by user, keep internal pointer
static bool automationEventRecording = false;           // Recording automation events flag
static unsigned int automationEventBaseFrame = 0;       // Automation events base frame, events recorded from this frame on
#endif

// Vr stereo mode state (rlvk has no stereo render path, see header notes)
static VrStereoConfig vrStereoConfig = { 0 };
static bool vrStereoEnabled = false;

//----------------------------------------------------------------------------------
// Module specific functions/data declared in other modules
//----------------------------------------------------------------------------------
#if SUPPORT_MODULE_RTEXT
extern void LoadFontDefault(void);      // [Module: rtext] Load default font
extern void UnloadFontDefault(void);    // [Module: rtext] Unload default font
#endif

//----------------------------------------------------------------------------------
// Module Internal Functions Declaration
//----------------------------------------------------------------------------------
static void InitTimer(void);                                // Initialize the time base for GetTime()
static void SetupViewport(int width, int height);           // Set viewport for a provided width and height
static void SetupShaderDefaultLocations(Shader *shader);    // Fill shader locs array with conventional locations

static void ScanDirectoryFiles(const char *basePath, FilePathList *list, const char *filter, unsigned int capacity);   // Scan all files and directories in a base path
static void ScanDirectoryFilesRecursively(const char *basePath, FilePathList *list, const char *filter, unsigned int capacity);  // Scan all files and directories recursively

#if SUPPORT_GESTURES_SYSTEM
static void UpdateGestureEvents(void);                      // Feed the gestures system from current touch/mouse state
#endif
#if SUPPORT_AUTOMATION_EVENTS
static void RecordAutomationEvent(void);                    // Record frame input events (automation events)
#endif

//----------------------------------------------------------------------------------
// Module Functions Definition: Window and Graphics Device
//----------------------------------------------------------------------------------

// Initialize window and rendering context
// NOTE: Window flags must be set beforehand with SetConfigFlags()
void InitWindow(int width, int height, const char *title)
{
    TRACELOG(LOG_INFO, "Initializing rayvulkan %s (raylib API %s)", "1.0", RAYLIB_VERSION);
    TRACELOG(LOG_INFO, "Supported raylib modules:");
    TRACELOG(LOG_INFO, "    > rcore:..... loaded (mandatory)");
    TRACELOG(LOG_INFO, "    > rlvk:...... loaded (mandatory)");
#if SUPPORT_MODULE_RSHAPES
    TRACELOG(LOG_INFO, "    > rshapes:... loaded (optional)");
#else
    TRACELOG(LOG_INFO, "    > rshapes:... not loaded (optional)");
#endif
#if SUPPORT_MODULE_RTEXTURES
    TRACELOG(LOG_INFO, "    > rtextures:. loaded (optional)");
#else
    TRACELOG(LOG_INFO, "    > rtextures:. not loaded (optional)");
#endif
#if SUPPORT_MODULE_RTEXT
    TRACELOG(LOG_INFO, "    > rtext:..... loaded (optional)");
#else
    TRACELOG(LOG_INFO, "    > rtext:..... not loaded (optional)");
#endif
#if SUPPORT_MODULE_RMODELS
    TRACELOG(LOG_INFO, "    > rmodels:... loaded (optional)");
#else
    TRACELOG(LOG_INFO, "    > rmodels:... not loaded (optional)");
#endif
#if SUPPORT_MODULE_RAUDIO
    TRACELOG(LOG_INFO, "    > raudio:.... loaded (optional)");
#else
    TRACELOG(LOG_INFO, "    > raudio:.... not loaded (optional)");
#endif

    if ((width <= 0) || (height <= 0))
    {
        TRACELOG(LOG_WARNING, "WINDOW: Invalid window dimensions provided (%i x %i), using 800x450", width, height);
        width = 800;
        height = 450;
    }

    // Initialize window data
    CORE.window.screen.width = width;
    CORE.window.screen.height = height;
    CORE.window.eventWaiting = false;
    CORE.window.screenMin.width = 0;
    CORE.window.screenMin.height = 0;
    CORE.window.screenMax.width = 0;
    CORE.window.screenMax.height = 0;
    CORE.window.scale.x = 1.0f;
    CORE.window.scale.y = 1.0f;
    CORE.window.title = ((title != NULL) && (title[0] != '\0'))? title : " ";

    // Initialize global input state
    memset(&CORE.input, 0, sizeof(CORE.input));
    CORE.input.keyboard.exitKey = KEY_ESCAPE;
    CORE.input.mouse.scale.x = 1.0f;
    CORE.input.mouse.scale.y = 1.0f;
    CORE.input.mouse.cursor = MOUSE_CURSOR_ARROW;
    CORE.input.gamepad.lastButtonPressed = GAMEPAD_BUTTON_UNKNOWN;

    // Initialize platform: native window + rlvk device/swapchain
    if (InitPlatform() != 0)
    {
        TRACELOG(LOG_WARNING, "SYSTEM: Failed to initialize platform");
        return;
    }

    // Setup default viewport (uses the actual framebuffer size, HiDPI aware)
    if ((CORE.window.render.width <= 0) || (CORE.window.render.height <= 0))
    {
        CORE.window.render.width = CORE.window.screen.width;
        CORE.window.render.height = CORE.window.screen.height;
    }
    SetupViewport(CORE.window.render.width, CORE.window.render.height);

#if SUPPORT_MODULE_RTEXT
    LoadFontDefault();
    #if SUPPORT_MODULE_RSHAPES
    // Set font white rectangle for shapes drawing, so shapes and text can be batched
    // together. Upstream raylib's 224-glyph default font keeps a solid white block at
    // glyph 95; our embedded font has 95 glyphs and no solid texel, so shapes fall back
    // to the renderer's default white texture (resolved lazily by rshapes) instead.
    Font defaultFont = GetFontDefault();
    if (defaultFont.glyphCount > 95)
    {
        Rectangle rec = defaultFont.recs[95];
        if ((CORE.window.flags & FLAG_MSAA_4X_HINT) > 0)
        {
            // NOTE: Maximize rec padding to avoid pixel bleeding on MSAA filtering
            SetShapesTexture(defaultFont.texture, (Rectangle){ rec.x + 2, rec.y + 2, 1, 1 });
        }
        else SetShapesTexture(defaultFont.texture, (Rectangle){ rec.x + 1, rec.y + 1, 1, 1 });
    }
    else SetShapesTexture((Texture2D){ 0 }, (Rectangle){ 0 });
    #endif
#else
    #if SUPPORT_MODULE_RSHAPES
    // Set default texture and rectangle to be used for shapes drawing
    Texture2D texture = { rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    SetShapesTexture(texture, (Rectangle){ 0.0f, 0.0f, 1.0f, 1.0f });
    #endif
#endif

    CORE.time.frameCounter = 0;
    CORE.window.shouldClose = false;
    CORE.window.ready = true;

    InitTimer();

    // Initialize random seed from current time
    SetRandomSeed((unsigned int)time(NULL));

    TRACELOG(LOG_INFO, "SYSTEM: Working Directory: %s", GetWorkingDirectory());
}

// Close window and unload rendering context
void CloseWindow(void)
{
    if (!CORE.window.ready) return;

#if SUPPORT_MODULE_RTEXT
    UnloadFontDefault();
#endif

    // NOTE: rlvk must release all Vulkan objects before the platform destroys the
    // surface/window it was created from (see rcore_internal.h ClosePlatform note)
    rlvkClose();
    ClosePlatform();

    if (CORE.window.dropFilepaths != NULL)
    {
        for (unsigned int i = 0; i < CORE.window.dropFileCount; i++) RL_FREE(CORE.window.dropFilepaths[i]);
        RL_FREE(CORE.window.dropFilepaths);
        CORE.window.dropFilepaths = NULL;
        CORE.window.dropFileCount = 0;
    }

    CORE.window.ready = false;
    TRACELOG(LOG_INFO, "Window closed successfully");
}

// Check if application should close
// NOTE: Returns true when window is not ready, so a naive main loop can not spin forever
bool WindowShouldClose(void)
{
    if (!CORE.window.ready) return true;

    // Check exit key (platform backends only set CORE.window.shouldClose on close requests)
    if ((CORE.input.keyboard.exitKey != KEY_NULL) && IsKeyPressed(CORE.input.keyboard.exitKey)) CORE.window.shouldClose = true;

    return CORE.window.shouldClose;
}

// Check if window has been initialized successfully
bool IsWindowReady(void) { return CORE.window.ready; }

// Check if window is currently fullscreen
bool IsWindowFullscreen(void) { return CORE.window.fullscreen; }

// Check if window is currently hidden
bool IsWindowHidden(void) { return ((CORE.window.flags & FLAG_WINDOW_HIDDEN) > 0); }

// Check if window is currently minimized
bool IsWindowMinimized(void) { return ((CORE.window.flags & FLAG_WINDOW_MINIMIZED) > 0); }

// Check if window is currently maximized
bool IsWindowMaximized(void) { return ((CORE.window.flags & FLAG_WINDOW_MAXIMIZED) > 0); }

// Check if window is currently focused
bool IsWindowFocused(void) { return ((CORE.window.flags & FLAG_WINDOW_UNFOCUSED) == 0); }

// Check if window has been resized last frame
bool IsWindowResized(void) { return CORE.window.resizedLastFrame; }

// Check if one specific window flag is enabled
bool IsWindowState(unsigned int flag) { return ((CORE.window.flags & flag) > 0); }

// Set window configuration state using flags
void SetWindowState(unsigned int flags)
{
    if (!CORE.window.ready)
    {
        // Window not created yet: just register the requested configuration
        CORE.window.flags |= flags;
        return;
    }

    // Only apply the flags that are not already set
    unsigned int toApply = flags & ~CORE.window.flags;
    if (toApply == 0) return;

    SetWindowStatePlatform(toApply);
    CORE.window.flags |= toApply;

    if ((toApply & FLAG_VSYNC_HINT) > 0) rlvkSetVSync(true);
    if ((toApply & FLAG_FULLSCREEN_MODE) > 0) CORE.window.fullscreen = true;
}

// Clear window configuration state flags
void ClearWindowState(unsigned int flags)
{
    if (!CORE.window.ready)
    {
        CORE.window.flags &= ~flags;
        return;
    }

    // Only clear the flags that are currently set
    unsigned int toClear = flags & CORE.window.flags;
    if (toClear == 0) return;

    ClearWindowStatePlatform(toClear);
    CORE.window.flags &= ~toClear;

    if ((toClear & FLAG_VSYNC_HINT) > 0) rlvkSetVSync(false);
    if ((toClear & FLAG_FULLSCREEN_MODE) > 0) CORE.window.fullscreen = false;
}

// Toggle window state: fullscreen/windowed
void ToggleFullscreen(void)
{
    if (!CORE.window.ready) return;

    ToggleFullscreenPlatform();

    CORE.window.fullscreen = !CORE.window.fullscreen;
    if (CORE.window.fullscreen) CORE.window.flags |= FLAG_FULLSCREEN_MODE;
    else CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;
}

// Toggle window state: borderless windowed
void ToggleBorderlessWindowed(void)
{
    if (!CORE.window.ready) return;

    ToggleBorderlessWindowedPlatform();

    if ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0) CORE.window.flags &= ~FLAG_BORDERLESS_WINDOWED_MODE;
    else CORE.window.flags |= FLAG_BORDERLESS_WINDOWED_MODE;
}

// Set window state: maximized, if resizable
void MaximizeWindow(void)
{
    if (!CORE.window.ready) return;

    MaximizeWindowPlatform();

    CORE.window.flags |= FLAG_WINDOW_MAXIMIZED;
    CORE.window.flags &= ~FLAG_WINDOW_MINIMIZED;
}

// Set window state: minimized
void MinimizeWindow(void)
{
    if (!CORE.window.ready) return;

    MinimizeWindowPlatform();

    CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
    CORE.window.flags &= ~FLAG_WINDOW_MAXIMIZED;
}

// Restore window from being minimized/maximized
void RestoreWindow(void)
{
    if (!CORE.window.ready) return;

    RestoreWindowPlatform();

    CORE.window.flags &= ~(FLAG_WINDOW_MINIMIZED | FLAG_WINDOW_MAXIMIZED);
}

// Set icon for window (single image, RGBA 32bit)
void SetWindowIcon(Image image)
{
    if (!CORE.window.ready) return;
    SetWindowIconPlatform(&image, 1);
}

// Set icon for window (multiple images, RGBA 32bit)
void SetWindowIcons(Image *images, int count)
{
    if (!CORE.window.ready) return;

    if ((images == NULL) || (count <= 0))
    {
        // Restore the platform default icon
        SetWindowIconPlatform(NULL, 0);
        return;
    }

    SetWindowIconPlatform(images, count);
}

// Set title for window
void SetWindowTitle(const char *title)
{
    CORE.window.title = title;
    if (CORE.window.ready) SetWindowTitlePlatform(title);
}

// Set window position on screen
void SetWindowPosition(int x, int y)
{
    if (!CORE.window.ready) return;

    SetWindowPositionPlatform(x, y);
    CORE.window.position.x = x;
    CORE.window.position.y = y;
}

// Set monitor for the current window
void SetWindowMonitor(int monitor)
{
    if (!CORE.window.ready) return;
    SetWindowMonitorPlatform(monitor);
}

// Set window minimum dimensions (FLAG_WINDOW_RESIZABLE)
void SetWindowMinSize(int width, int height)
{
    CORE.window.screenMin.width = width;
    CORE.window.screenMin.height = height;
    if (CORE.window.ready) SetWindowMinSizePlatform(width, height);
}

// Set window maximum dimensions (FLAG_WINDOW_RESIZABLE)
void SetWindowMaxSize(int width, int height)
{
    CORE.window.screenMax.width = width;
    CORE.window.screenMax.height = height;
    if (CORE.window.ready) SetWindowMaxSizePlatform(width, height);
}

// Set window dimensions
void SetWindowSize(int width, int height)
{
    if (!CORE.window.ready)
    {
        CORE.window.screen.width = width;
        CORE.window.screen.height = height;
        return;
    }

    SetWindowSizePlatform(width, height);
}

// Set window opacity [0.0f..1.0f]
void SetWindowOpacity(float opacity)
{
    if (!CORE.window.ready) return;

    if (opacity > 1.0f) opacity = 1.0f;
    else if (opacity < 0.0f) opacity = 0.0f;

    SetWindowOpacityPlatform(opacity);
}

// Set window focused
void SetWindowFocused(void)
{
    if (!CORE.window.ready) return;
    SetWindowFocusedPlatform();
}

// Get native window handle
void *GetWindowHandle(void)
{
    if (!CORE.window.ready) return NULL;
    return GetWindowHandlePlatform();
}

// Get current screen width
int GetScreenWidth(void) { return CORE.window.screen.width; }

// Get current screen height
int GetScreenHeight(void) { return CORE.window.screen.height; }

// Get current render width (considers HiDPI)
int GetRenderWidth(void) { return CORE.window.render.width; }

// Get current render height (considers HiDPI)
int GetRenderHeight(void) { return CORE.window.render.height; }

// Get number of connected monitors
int GetMonitorCount(void) { return GetMonitorCountPlatform(); }

// Get current monitor where window is placed
int GetCurrentMonitor(void) { return GetCurrentMonitorPlatform(); }

// Get selected monitor position
Vector2 GetMonitorPosition(int monitor) { return GetMonitorPositionPlatform(monitor); }

// Get selected monitor width (currently used by monitor)
int GetMonitorWidth(int monitor) { return GetMonitorWidthPlatform(monitor); }

// Get selected monitor height (currently used by monitor)
int GetMonitorHeight(int monitor) { return GetMonitorHeightPlatform(monitor); }

// Get selected monitor physical width in millimetres
int GetMonitorPhysicalWidth(int monitor) { return GetMonitorPhysicalWidthPlatform(monitor); }

// Get selected monitor physical height in millimetres
int GetMonitorPhysicalHeight(int monitor) { return GetMonitorPhysicalHeightPlatform(monitor); }

// Get selected monitor refresh rate
int GetMonitorRefreshRate(int monitor) { return GetMonitorRefreshRatePlatform(monitor); }

// Get the human-readable, UTF-8 encoded name of the selected monitor
const char *GetMonitorName(int monitor)
{
    const char *name = GetMonitorNamePlatform(monitor);
    return (name != NULL)? name : "";
}

// Get window position XY on monitor
Vector2 GetWindowPosition(void)
{
    return (Vector2){ (float)CORE.window.position.x, (float)CORE.window.position.y };
}

// Get window scale DPI factor for current monitor
Vector2 GetWindowScaleDPI(void)
{
    if (!CORE.window.ready) return (Vector2){ 1.0f, 1.0f };
    return GetWindowScaleDPIPlatform();
}

// Set clipboard text content
void SetClipboardText(const char *text)
{
    if (text == NULL) return;
    SetClipboardTextPlatform(text);
}

// Get clipboard text content
const char *GetClipboardText(void)
{
    const char *text = GetClipboardTextPlatform();
    return (text != NULL)? text : "";
}

// Get clipboard image content
Image GetClipboardImage(void)
{
#if SUPPORT_CLIPBOARD_IMAGE
    return GetClipboardImagePlatform();
#else
    TRACELOG(LOG_WARNING, "SYSTEM: Clipboard image support not enabled (SUPPORT_CLIPBOARD_IMAGE)");
    Image image = { 0 };
    return image;
#endif
}

// Enable waiting for events on EndDrawing(), no automatic event polling
void EnableEventWaiting(void) { CORE.window.eventWaiting = true; }

// Disable waiting for events on EndDrawing(), automatic events polling
void DisableEventWaiting(void) { CORE.window.eventWaiting = false; }

//----------------------------------------------------------------------------------
// Module Functions Definition: Cursor
//----------------------------------------------------------------------------------

// Show mouse cursor
void ShowCursor(void)
{
    ShowCursorPlatform();
    CORE.input.mouse.cursorHidden = false;
}

// Hide mouse cursor
void HideCursor(void)
{
    HideCursorPlatform();
    CORE.input.mouse.cursorHidden = true;
}

// Check if cursor is not visible
bool IsCursorHidden(void) { return CORE.input.mouse.cursorHidden; }

// Enable cursor (unlock cursor)
void EnableCursor(void)
{
    EnableCursorPlatform();

    // Set cursor position in the middle
    SetMousePosition(CORE.window.screen.width/2, CORE.window.screen.height/2);

    CORE.input.mouse.cursorHidden = false;
    CORE.input.mouse.cursorLocked = false;
}

// Disable cursor (lock cursor)
void DisableCursor(void)
{
    DisableCursorPlatform();

    // Set cursor position in the middle
    SetMousePosition(CORE.window.screen.width/2, CORE.window.screen.height/2);

    CORE.input.mouse.cursorHidden = true;
    CORE.input.mouse.cursorLocked = true;
}

// Check if cursor is on the current screen
bool IsCursorOnScreen(void) { return CORE.input.mouse.cursorOnScreen; }

//----------------------------------------------------------------------------------
// Module Functions Definition: Drawing
//----------------------------------------------------------------------------------

// Set background color (framebuffer clear color)
void ClearBackground(Color color)
{
    rlClearColor(color.r, color.g, color.b, color.a);
    rlClearScreenBuffers();      // Clear current framebuffers
}

// Setup canvas (framebuffer) to start drawing
void BeginDrawing(void)
{
    CORE.time.current = GetTime();
    CORE.time.update = CORE.time.current - CORE.time.previous;
    CORE.time.previous = CORE.time.current;

    rlvkBeginFrame();            // Acquire swapchain image, begin frame command buffer

    rlLoadIdentity();            // Reset current matrix (modelview)
}

// End canvas drawing and swap buffers (double buffering)
void EndDrawing(void)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch

#if SUPPORT_GESTURES_SYSTEM && SUPPORT_MOUSE_GESTURES
    // NOTE: Gestures are updated on PollInputEvents()
#endif

#if SUPPORT_SCREEN_CAPTURE
    if (IsKeyPressed(KEY_F12))
    {
        TakeScreenshot(TextFormat("screenshot%03i.png", screenshotCounter));
        screenshotCounter++;
    }
#endif

#if SUPPORT_AUTOMATION_EVENTS
    if (automationEventRecording) RecordAutomationEvent();
#endif

#if !SUPPORT_CUSTOM_FRAME_CONTROL
    SwapScreenBuffer();          // Copy back buffer to front buffer (screen)

    // Frame time control system
    CORE.time.current = GetTime();
    CORE.time.draw = CORE.time.current - CORE.time.previous;
    CORE.time.previous = CORE.time.current;

    CORE.time.frame = CORE.time.update + CORE.time.draw;

    // Wait for some milliseconds...
    if (CORE.time.frame < CORE.time.target)
    {
        WaitTime(CORE.time.target - CORE.time.frame);

        CORE.time.current = GetTime();
        double waitTime = CORE.time.current - CORE.time.previous;
        CORE.time.previous = CORE.time.current;

        CORE.time.frame += waitTime;    // Total frame time: update + draw + wait
    }

    PollInputEvents();           // Poll user events (before next frame update)
#endif

    CORE.time.frameCounter++;
}

// Initialize 2D mode with custom camera (2D)
void BeginMode2D(Camera2D camera)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch

    rlLoadIdentity();            // Reset current matrix (modelview)

    // Apply 2d camera transformation to modelview
    rlMultMatrixf(MatrixToFloat(GetCameraMatrix2D(camera)));
}

// Ends 2D mode with custom camera
void EndMode2D(void)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch

    rlLoadIdentity();            // Reset current matrix (modelview)
}

// Initializes 3D mode with custom camera (3D)
void BeginMode3D(Camera camera)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch

    rlMatrixMode(RL_PROJECTION); // Switch to projection matrix
    rlPushMatrix();              // Save previous matrix, which contains the settings for the 2d ortho projection
    rlLoadIdentity();            // Reset current matrix (projection)

    float aspect = (float)rlGetFramebufferWidth()/(float)rlGetFramebufferHeight();

    // NOTE: zNear and zFar values are important when computing depth buffer values
    if (camera.projection == CAMERA_PERSPECTIVE)
    {
        // Setup perspective projection
        double top = rlGetCullDistanceNear()*tan(camera.fovy*0.5*DEG2RAD);
        double right = top*aspect;

        rlFrustum(-right, right, -top, top, rlGetCullDistanceNear(), rlGetCullDistanceFar());
    }
    else if (camera.projection == CAMERA_ORTHOGRAPHIC)
    {
        // Setup orthographic projection
        double top = camera.fovy/2.0;
        double right = top*aspect;

        rlOrtho(-right, right, -top, top, rlGetCullDistanceNear(), rlGetCullDistanceFar());
    }

    rlMatrixMode(RL_MODELVIEW);  // Switch back to modelview matrix
    rlLoadIdentity();            // Reset current matrix (modelview)

    // Setup Camera view
    Matrix matView = MatrixLookAt(camera.position, camera.target, camera.up);
    rlMultMatrixf(MatrixToFloat(matView));      // Multiply modelview matrix by view matrix (camera)

    rlEnableDepthTest();         // Enable DEPTH_TEST for 3D
}

// Ends 3D mode and returns to default 2D orthographic mode
void EndMode3D(void)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch

    rlMatrixMode(RL_PROJECTION); // Switch to projection matrix
    rlPopMatrix();               // Restore previous matrix (projection) from matrix stack

    rlMatrixMode(RL_MODELVIEW);  // Switch back to modelview matrix
    rlLoadIdentity();            // Reset current matrix (modelview)

    rlDisableDepthTest();        // Disable DEPTH_TEST for 2D
}

// Initializes render texture for drawing
void BeginTextureMode(RenderTexture2D target)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch

    rlEnableFramebuffer(target.id); // Enable render target

    // Set viewport and RLGL internal framebuffer size
    rlViewport(0, 0, target.texture.width, target.texture.height);
    rlSetFramebufferWidth(target.texture.width);
    rlSetFramebufferHeight(target.texture.height);

    rlMatrixMode(RL_PROJECTION); // Switch to projection matrix
    rlLoadIdentity();            // Reset current matrix (projection)

    // Set orthographic projection to current framebuffer size
    // NOTE: Configured top-left corner as (0, 0)
    rlOrtho(0, target.texture.width, target.texture.height, 0, 0.0f, 1.0f);

    rlMatrixMode(RL_MODELVIEW);  // Switch back to modelview matrix
    rlLoadIdentity();            // Reset current matrix (modelview)

    CORE.window.usingFbo = true;
}

// Ends drawing to render texture
void EndTextureMode(void)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch

    rlDisableFramebuffer();      // Disable render target (fbo)

    // Set viewport to default framebuffer size
    rlSetFramebufferWidth(CORE.window.render.width);
    rlSetFramebufferHeight(CORE.window.render.height);
    SetupViewport(CORE.window.render.width, CORE.window.render.height);

    CORE.window.usingFbo = false;
}

// Begin custom shader mode
void BeginShaderMode(Shader shader)
{
    rlSetShader(shader.id, shader.locs);
}

// End custom shader mode (returns to default shader)
void EndShaderMode(void)
{
    rlSetShader(rlGetShaderIdDefault(), rlGetShaderLocsDefault());
}

// Begin blending mode (alpha, additive, multiplied, subtract, custom)
// NOTE: Blend modes supported are enumerated in BlendMode enum
void BeginBlendMode(int mode)
{
    rlSetBlendMode(mode);
}

// End blending mode (reset to default: alpha blending)
void EndBlendMode(void)
{
    rlSetBlendMode(BLEND_ALPHA);
}

// Begin scissor mode (define screen area for following drawing)
// NOTE: Scissor rec refers to bottom-left corner, we change it to upper-left
void BeginScissorMode(int x, int y, int width, int height)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch

    rlEnableScissorTest();

    if (!CORE.window.usingFbo && ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0))
    {
        Vector2 scale = GetWindowScaleDPI();
        rlScissor((int)(x*scale.x), (int)(rlGetFramebufferHeight() - (y + height)*scale.y), (int)(width*scale.x), (int)(height*scale.y));
    }
    else rlScissor(x, rlGetFramebufferHeight() - (y + height), width, height);
}

// End scissor mode
void EndScissorMode(void)
{
    rlDrawRenderBatchActive();   // Update and draw internal render batch
    rlDisableScissorTest();
}

// Begin stereo rendering (requires VR simulator)
// NOTE: rlvk provides no stereo render path (no dual-viewport pipeline), so the
// configuration is applied for the LEFT eye only and a warning is emitted once
void BeginVrStereoMode(VrStereoConfig config)
{
    static bool vrWarned = false;
    if (!vrWarned)
    {
        TRACELOG(LOG_WARNING, "VR: Stereo rendering is not supported by the Vulkan backend, rendering left eye only");
        vrWarned = true;
    }

    rlDrawRenderBatchActive();   // Update and draw internal render batch

    vrStereoConfig = config;
    vrStereoEnabled = true;

    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlSetMatrixProjection(config.projection[0]);

    rlMatrixMode(RL_MODELVIEW);
    rlPushMatrix();
    rlMultMatrixf(MatrixToFloat(config.viewOffset[0]));
}

// End stereo rendering (requires VR simulator)
void EndVrStereoMode(void)
{
    if (!vrStereoEnabled) return;

    rlDrawRenderBatchActive();   // Update and draw internal render batch

    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();

    rlMatrixMode(RL_MODELVIEW);
    rlPopMatrix();

    vrStereoEnabled = false;
}

// Load VR stereo config for VR simulator device parameters
VrStereoConfig LoadVrStereoConfig(VrDeviceInfo device)
{
    VrStereoConfig config = { 0 };

    // Compute aspect ratio
    float aspect = ((float)device.hResolution*0.5f)/(float)device.vResolution;

    // Compute lens parameters
    float lensShift = (device.hScreenSize*0.25f - device.lensSeparationDistance*0.5f)/device.hScreenSize;
    config.leftLensCenter[0] = 0.25f + lensShift;
    config.leftLensCenter[1] = 0.5f;
    config.rightLensCenter[0] = 0.75f - lensShift;
    config.rightLensCenter[1] = 0.5f;
    config.leftScreenCenter[0] = 0.25f;
    config.leftScreenCenter[1] = 0.5f;
    config.rightScreenCenter[0] = 0.75f;
    config.rightScreenCenter[1] = 0.5f;

    // Compute distortion scale parameters
    // NOTE: To get lens max radius, lensShift must be normalized to [-1..1]
    float lensRadius = fabsf(-1.0f - 4.0f*lensShift);
    float lensRadiusSq = lensRadius*lensRadius;
    float distortionScale = device.lensDistortionValues[0] +
                            device.lensDistortionValues[1]*lensRadiusSq +
                            device.lensDistortionValues[2]*lensRadiusSq*lensRadiusSq +
                            device.lensDistortionValues[3]*lensRadiusSq*lensRadiusSq*lensRadiusSq;

    float normScreenWidth = 0.5f;
    float normScreenHeight = 1.0f;
    config.scaleIn[0] = 2.0f/normScreenWidth;
    config.scaleIn[1] = 2.0f/normScreenHeight/aspect;
    config.scale[0] = normScreenWidth*0.5f/distortionScale;
    config.scale[1] = normScreenHeight*0.5f*aspect/distortionScale;

    // Fovy is normally computed with: 2*atan2f(device.vScreenSize, 2*device.eyeToScreenDistance)
    // ...but with lens distortion it is increased (see Oculus SDK Documentation)
    float fovy = 2.0f*atan2f(device.vScreenSize*0.5f*distortionScale, device.eyeToScreenDistance);

    // Compute camera projection matrices
    float projOffset = 4.0f*lensShift;      // Scaled to projection space coordinates [-1..1]
    Matrix proj = MatrixPerspective(fovy, aspect, rlGetCullDistanceNear(), rlGetCullDistanceFar());

    config.projection[0] = MatrixMultiply(proj, MatrixTranslate(projOffset, 0.0f, 0.0f));
    config.projection[1] = MatrixMultiply(proj, MatrixTranslate(-projOffset, 0.0f, 0.0f));

    // Compute camera transformation matrices
    // NOTE: Camera movement might seem more natural if we model the head
    // Our axis of rotation is the base of our head, so we might want to add
    // some y (base of head to eye level) and -z (center of head to eye protrusion) to the camera positions
    config.viewOffset[0] = MatrixTranslate(-device.interpupillaryDistance*0.5f, 0.075f, 0.045f);
    config.viewOffset[1] = MatrixTranslate(device.interpupillaryDistance*0.5f, 0.075f, 0.045f);

    // Compute eyes Viewports
    //config.eyeViewportRight[0] = 0;
    //config.eyeViewportRight[1] = 0;
    //config.eyeViewportRight[2] = device.hResolution/2;
    //config.eyeViewportRight[3] = device.vResolution;

    return config;
}

// Unload VR stereo config properties
void UnloadVrStereoConfig(VrStereoConfig config)
{
    (void)config;
    // NOTE: No resources are allocated by LoadVrStereoConfig()
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Shaders management
//----------------------------------------------------------------------------------

// Load shader from files and bind default locations
// WARNING: On this backend shader files MUST contain SPIR-V binaries (compiled
// offline with glslc/glslangValidator), GLSL source is not compiled at runtime
Shader LoadShader(const char *vsFileName, const char *fsFileName)
{
    Shader shader = { 0 };

    unsigned char *vsSpirv = NULL;
    unsigned char *fsSpirv = NULL;
    int vsSize = 0;
    int fsSize = 0;

    if (vsFileName != NULL) vsSpirv = LoadFileData(vsFileName, &vsSize);
    if (fsFileName != NULL) fsSpirv = LoadFileData(fsFileName, &fsSize);

    shader.id = rlLoadShaderCodeSize(vsSpirv, vsSize, fsSpirv, fsSize);

    UnloadFileData(vsSpirv);
    UnloadFileData(fsSpirv);

    if (shader.id == rlGetShaderIdDefault()) shader.locs = rlGetShaderLocsDefault();
    else if (shader.id > 0) SetupShaderDefaultLocations(&shader);
    else TRACELOG(LOG_WARNING, "SHADER: Failed to load shader code [%s, %s]",
                  (vsFileName != NULL)? vsFileName : "internal", (fsFileName != NULL)? fsFileName : "internal");

    return shader;
}

// Load shader from code strings and bind default locations
// NOTE: Code is expected to be a SPIR-V binary blob (see LoadShader notes)
Shader LoadShaderFromMemory(const char *vsCode, const char *fsCode)
{
    Shader shader = { 0 };

    shader.id = rlLoadShaderCode(vsCode, fsCode);

    if (shader.id == rlGetShaderIdDefault()) shader.locs = rlGetShaderLocsDefault();
    else if (shader.id > 0) SetupShaderDefaultLocations(&shader);
    else TRACELOG(LOG_WARNING, "SHADER: Failed to load shader code from memory");

    return shader;
}

// Check if a shader is valid (loaded on GPU)
bool IsShaderValid(Shader shader)
{
    return ((shader.id > 0) && (shader.locs != NULL));
}

// Get shader uniform location
int GetShaderLocation(Shader shader, const char *uniformName)
{
    return rlGetLocationUniform(shader.id, uniformName);
}

// Get shader attribute location
int GetShaderLocationAttrib(Shader shader, const char *attribName)
{
    return rlGetLocationAttrib(shader.id, attribName);
}

// Set shader uniform value
void SetShaderValue(Shader shader, int locIndex, const void *value, int uniformType)
{
    SetShaderValueV(shader, locIndex, value, uniformType, 1);
}

// Set shader uniform value vector
void SetShaderValueV(Shader shader, int locIndex, const void *value, int uniformType, int count)
{
    if (locIndex < 0) return;

    rlEnableShader(shader.id);
    rlSetUniform(locIndex, value, uniformType, count);
}

// Set shader uniform value (matrix 4x4)
void SetShaderValueMatrix(Shader shader, int locIndex, Matrix mat)
{
    if (locIndex < 0) return;

    rlEnableShader(shader.id);
    rlSetUniformMatrix(locIndex, mat);
}

// Set shader uniform value for texture
void SetShaderValueTexture(Shader shader, int locIndex, Texture2D texture)
{
    if (locIndex < 0) return;

    rlEnableShader(shader.id);
    rlSetUniformSampler(locIndex, texture.id);
}

// Unload shader from GPU memory (VRAM)
void UnloadShader(Shader shader)
{
    if (shader.id == rlGetShaderIdDefault()) return;

    rlUnloadShaderProgram(shader.id);

    // NOTE: If shader loading failed, it should be 0
    if ((shader.locs != NULL) && (shader.locs != rlGetShaderLocsDefault())) RL_FREE(shader.locs);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Screen-space queries
//----------------------------------------------------------------------------------

// Get a ray trace from screen position (i.e mouse)
Ray GetScreenToWorldRay(Vector2 position, Camera camera)
{
    return GetScreenToWorldRayEx(position, camera, GetScreenWidth(), GetScreenHeight());
}

// Get a ray trace from screen position (i.e mouse) in a viewport
Ray GetScreenToWorldRayEx(Vector2 position, Camera camera, int width, int height)
{
    Ray ray = { 0 };

    // Calculate normalized device coordinates
    // NOTE: y value is negative
    float x = (2.0f*position.x)/(float)width - 1.0f;
    float y = 1.0f - (2.0f*position.y)/(float)height;
    float z = 1.0f;

    // Store values in a vector
    Vector3 deviceCoords = { x, y, z };

    // Calculate view matrix from camera look at
    Matrix matView = MatrixLookAt(camera.position, camera.target, camera.up);

    Matrix matProj = MatrixIdentity();

    if (camera.projection == CAMERA_PERSPECTIVE)
    {
        // Calculate projection matrix from perspective
        matProj = MatrixPerspective(camera.fovy*DEG2RAD, ((double)width/(double)height), rlGetCullDistanceNear(), rlGetCullDistanceFar());
    }
    else if (camera.projection == CAMERA_ORTHOGRAPHIC)
    {
        double aspect = (double)width/(double)height;
        double top = camera.fovy/2.0;
        double right = top*aspect;

        // Calculate projection matrix from orthographic
        matProj = MatrixOrtho(-right, right, -top, top, 0.01, 1000.0);
    }

    // Unproject far/near points
    Vector3 nearPoint = Vector3Unproject((Vector3){ deviceCoords.x, deviceCoords.y, 0.0f }, matProj, matView);
    Vector3 farPoint = Vector3Unproject((Vector3){ deviceCoords.x, deviceCoords.y, 1.0f }, matProj, matView);

    // Unproject the mouse cursor in the near plane
    // We need this as the source position because orthographic projects, compared to perspective doesn't have a
    // convergence point, meaning that the "eye" of the camera is more like a plane than a point
    Vector3 cameraPlanePointerPos = Vector3Unproject((Vector3){ deviceCoords.x, deviceCoords.y, -1.0f }, matProj, matView);

    // Calculate normalized direction vector
    Vector3 direction = Vector3Normalize(Vector3Subtract(farPoint, nearPoint));

    if (camera.projection == CAMERA_PERSPECTIVE) ray.position = camera.position;
    else if (camera.projection == CAMERA_ORTHOGRAPHIC) ray.position = cameraPlanePointerPos;

    // Apply calculated vectors to ray
    ray.direction = direction;

    return ray;
}

// Get transform matrix for camera
Matrix GetCameraMatrix(Camera camera)
{
    return MatrixLookAt(camera.position, camera.target, camera.up);
}

// Get camera 2d transform matrix
Matrix GetCameraMatrix2D(Camera2D camera)
{
    Matrix matTransform = { 0 };

    // The camera in world-space is set by
    //   1. Move it to target
    //   2. Rotate by -rotation and scale by (1/zoom)
    //      When setting higher scale, it's more intuitive for the world to become bigger (= camera become smaller),
    //      not for the camera getting bigger, hence the invert. Same deal with rotation
    //   3. Move it by (-offset);
    //      Offset defines target transform relative to screen, but since we're effectively "moving" screen (camera)
    //      we need to do it into opposite direction (inverse transform)

    // Having camera transform in world-space, inverse of it gives the modelview transform
    // Since (A*B*C)' = C'*B'*A', the modelview is
    //   1. Move to offset
    //   2. Rotate and Scale
    //   3. Move by -target
    Matrix matOrigin = MatrixTranslate(-camera.target.x, -camera.target.y, 0.0f);
    Matrix matRotation = MatrixRotate((Vector3){ 0.0f, 0.0f, 1.0f }, camera.rotation*DEG2RAD);
    Matrix matScale = MatrixScale(camera.zoom, camera.zoom, 1.0f);
    Matrix matTranslation = MatrixTranslate(camera.offset.x, camera.offset.y, 0.0f);

    matTransform = MatrixMultiply(MatrixMultiply(matOrigin, MatrixMultiply(matScale, matRotation)), matTranslation);

    return matTransform;
}

// Get the screen space position from a 3d world space position
Vector2 GetWorldToScreen(Vector3 position, Camera camera)
{
    return GetWorldToScreenEx(position, camera, GetScreenWidth(), GetScreenHeight());
}

// Get size position for a 3d world space position
Vector2 GetWorldToScreenEx(Vector3 position, Camera camera, int width, int height)
{
    // Calculate projection matrix (from perspective instead of frustum)
    Matrix matProj = MatrixIdentity();

    if (camera.projection == CAMERA_PERSPECTIVE)
    {
        // Calculate projection matrix from perspective
        matProj = MatrixPerspective(camera.fovy*DEG2RAD, ((double)width/(double)height), rlGetCullDistanceNear(), rlGetCullDistanceFar());
    }
    else if (camera.projection == CAMERA_ORTHOGRAPHIC)
    {
        double aspect = (double)width/(double)height;
        double top = camera.fovy/2.0;
        double right = top*aspect;

        // Calculate projection matrix from orthographic
        matProj = MatrixOrtho(-right, right, -top, top, rlGetCullDistanceNear(), rlGetCullDistanceFar());
    }

    // Calculate view matrix from camera look at (and transpose it)
    Matrix matView = MatrixLookAt(camera.position, camera.target, camera.up);

    // TODO: Why not use Vector3Transform(Vector3 v, Matrix mat)?

    // Convert world position vector to quaternion
    Quaternion worldPos = { position.x, position.y, position.z, 1.0f };

    // Transform world position to view
    worldPos = QuaternionTransform(worldPos, matView);

    // Transform result to projection (clip space position)
    worldPos = QuaternionTransform(worldPos, matProj);

    // Calculate normalized device coordinates (inverted y)
    Vector3 ndcPos = { worldPos.x/worldPos.w, -worldPos.y/worldPos.w, worldPos.z/worldPos.w };

    // Calculate 2d screen position vector
    Vector2 screenPosition = { (ndcPos.x + 1.0f)/2.0f*(float)width, (ndcPos.y + 1.0f)/2.0f*(float)height };

    return screenPosition;
}

// Get the screen space position for a 2d camera world space position
Vector2 GetWorldToScreen2D(Vector2 position, Camera2D camera)
{
    Matrix matCamera = GetCameraMatrix2D(camera);
    Vector3 transform = Vector3Transform((Vector3){ position.x, position.y, 0 }, matCamera);

    return (Vector2){ transform.x, transform.y };
}

// Get the world space position for a 2d camera screen space position
Vector2 GetScreenToWorld2D(Vector2 position, Camera2D camera)
{
    Matrix invMatCamera = MatrixInvert(GetCameraMatrix2D(camera));
    Vector3 transform = Vector3Transform((Vector3){ position.x, position.y, 0 }, invMatCamera);

    return (Vector2){ transform.x, transform.y };
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Timing
//----------------------------------------------------------------------------------

// Set target FPS (maximum)
void SetTargetFPS(int fps)
{
    if (fps < 1) CORE.time.target = 0.0;
    else CORE.time.target = 1.0/(double)fps;

    TRACELOG(LOG_INFO, "TIMER: Target time per frame: %02.03f milliseconds", (float)CORE.time.target*1000.0f);
}

// Get current FPS
// NOTE: We calculate an average of the last frames to smooth the value
int GetFPS(void)
{
    int fps = 0;

#if !SUPPORT_CUSTOM_FRAME_CONTROL
    #define FPS_CAPTURE_FRAMES_COUNT    30      // 30 captures
    #define FPS_AVERAGE_TIME_SECONDS   0.5f     // 500 milliseconds
    #define FPS_STEP (FPS_AVERAGE_TIME_SECONDS/FPS_CAPTURE_FRAMES_COUNT)

    static int index = 0;
    static float history[FPS_CAPTURE_FRAMES_COUNT] = { 0 };
    static float average = 0, last = 0;
    float fpsFrame = GetFrameTime();

    // If we have a freeze mode, we should not update the FPS
    if (fpsFrame == 0) return (int)roundf(average);

    if ((GetTime() - last) > FPS_STEP)
    {
        last = (float)GetTime();
        index = (index + 1)%FPS_CAPTURE_FRAMES_COUNT;
        average -= history[index];
        history[index] = fpsFrame/FPS_CAPTURE_FRAMES_COUNT;
        average += history[index];
    }

    if (average > 0.0f) fps = (int)roundf(1.0f/average);
#endif

    return fps;
}

// Get time in seconds for last frame drawn (delta time)
float GetFrameTime(void)
{
    return (float)CORE.time.frame;
}

// Get elapsed time measured in seconds since InitWindow()
double GetTime(void)
{
    return GetTimePlatform() - CORE.time.base;
}

// Swap back buffer with front buffer (screen drawing)
void SwapScreenBuffer(void)
{
    SwapScreenBufferPlatform();
}

// Wait for some time (stop program execution)
// NOTE: Sleep() granularity could be around 10 ms, it means, Sleep() could
// take longer than expected... for that reason we use a busy wait loop tail
void WaitTime(double seconds)
{
    if (seconds < 0) return;

#if SUPPORT_BUSY_WAIT_LOOP || SUPPORT_PARTIALBUSY_WAIT_LOOP
    double destinationTime = GetTime() + seconds;
#endif

#if SUPPORT_BUSY_WAIT_LOOP
    while (GetTime() < destinationTime) { }
#else
    #if SUPPORT_PARTIALBUSY_WAIT_LOOP
        double sleepSeconds = seconds - seconds*0.05;    // Reserve 5% of the time for the busy-wait tail
    #else
        double sleepSeconds = seconds;
    #endif

    if (sleepSeconds > 0.0) WaitTimePlatform(sleepSeconds);

    #if SUPPORT_PARTIALBUSY_WAIT_LOOP
        while (GetTime() < destinationTime) { }
    #endif
#endif
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Random values generation
//----------------------------------------------------------------------------------

// Set the seed for the random number generator
void SetRandomSeed(unsigned int seed)
{
#if SUPPORT_RPRAND_GENERATOR
    rprand_set_seed(seed);
#else
    srand(seed);
#endif
}

// Get a random value between min and max included
int GetRandomValue(int min, int max)
{
    int value = 0;

    if (min > max)
    {
        int tmp = max;
        max = min;
        min = tmp;
    }

#if SUPPORT_RPRAND_GENERATOR
    value = rprand_get_value(min, max);
#else
    // WARNING: Ranges higher than RAND_MAX will return invalid results
    // More specifically, if (max - min) > INT_MAX there will be an overflow,
    // and otherwise if (max - min) > RAND_MAX the random value will incorrectly never exceed a certain threshold
    if ((unsigned int)(max - min) > (unsigned int)RAND_MAX)
    {
        TRACELOG(LOG_WARNING, "Invalid GetRandomValue() arguments, range should not be higher than %i", RAND_MAX);
    }

    value = (rand()%(abs(max - min) + 1) + min);
#endif

    return value;
}

// Load random values sequence, no values repeated, sequence should be unloaded with UnloadRandomSequence()
int *LoadRandomSequence(unsigned int count, int min, int max)
{
    int *values = NULL;

#if SUPPORT_RPRAND_GENERATOR
    values = rprand_load_sequence(count, min, max);
#else
    if (count > ((unsigned int)abs(max - min) + 1)) return values;

    values = (int *)RL_CALLOC(count, sizeof(int));
    if (values == NULL) return NULL;

    int value = 0;
    bool dupValue = false;

    for (unsigned int i = 0; i < count;)
    {
        value = (rand()%(abs(max - min) + 1) + min);
        dupValue = false;

        for (unsigned int j = 0; j < i; j++)
        {
            if (values[j] == value)
            {
                dupValue = true;
                break;
            }
        }

        if (!dupValue)
        {
            values[i] = value;
            i++;
        }
    }
#endif

    return values;
}

// Unload random values sequence
void UnloadRandomSequence(int *sequence)
{
#if SUPPORT_RPRAND_GENERATOR
    rprand_unload_sequence(sequence);
#else
    RL_FREE(sequence);
#endif
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Misc
//----------------------------------------------------------------------------------

// Takes a screenshot of current screen
// NOTE: Provided fileName should not contain paths, saving to working directory
void TakeScreenshot(const char *fileName)
{
#if SUPPORT_MODULE_RTEXTURES
    if (!IsFileNameValid(GetFileName(fileName)))
    {
        TRACELOG(LOG_WARNING, "SCREENSHOT: Provided fileName is not valid: %s", fileName);
        return;
    }

    // Security check to (partially) avoid malicious code
    if (strchr(fileName, '\'') != NULL)
    {
        TRACELOG(LOG_WARNING, "SYSTEM: Provided fileName could be potentially malicious, avoid [\'] character");
        return;
    }

    unsigned char *imgData = rlReadScreenPixels(CORE.window.render.width, CORE.window.render.height);
    if (imgData == NULL)
    {
        TRACELOG(LOG_WARNING, "SCREENSHOT: Failed to read screen pixels");
        return;
    }

    Image image = { imgData, CORE.window.render.width, CORE.window.render.height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };

    if (ExportImage(image, fileName)) TRACELOG(LOG_INFO, "SYSTEM: [%s] Screenshot taken successfully", fileName);
    else TRACELOG(LOG_WARNING, "SYSTEM: [%s] Screenshot could not be saved", fileName);

    RL_FREE(imgData);
#else
    TRACELOG(LOG_WARNING, "IMAGE: ExportImage() requires module: rtextures");
    (void)fileName;
#endif
}

// Setup window configuration flags
// NOTE: This function is expected to be called before window creation,
// because it sets up some flags for the window creation process
void SetConfigFlags(unsigned int flags)
{
    // Selected flags are set but not evaluated at this point,
    // flag evaluation happens at InitWindow() or SetWindowState()
    CORE.window.flags |= flags;
}

// Open URL with default system browser (if available)
// NOTE: This function is only safe to use if you control the URL given
void OpenURL(const char *url)
{
    if (url == NULL) return;

    // Security check to (partially) avoid malicious code on target platform
    if (strchr(url, '\'') != NULL)
    {
        TRACELOG(LOG_WARNING, "SYSTEM: Provided URL could be potentially malicious, avoid [\'] character");
        return;
    }

    OpenURLPlatform(url);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Input handling — keyboard
//----------------------------------------------------------------------------------

// Check if a key has been pressed once
bool IsKeyPressed(int key)
{
    if ((key < 0) || (key >= MAX_KEYBOARD_KEYS)) return false;

    return ((CORE.input.keyboard.previousKeyState[key] == 0) && (CORE.input.keyboard.currentKeyState[key] == 1));
}

// Check if a key has been pressed again
bool IsKeyPressedRepeat(int key)
{
    if ((key < 0) || (key >= MAX_KEYBOARD_KEYS)) return false;

    return (CORE.input.keyboard.keyRepeatInFrame[key] == 1);
}

// Check if a key is being pressed (key held down)
bool IsKeyDown(int key)
{
    if ((key < 0) || (key >= MAX_KEYBOARD_KEYS)) return false;

    return (CORE.input.keyboard.currentKeyState[key] == 1);
}

// Check if a key has been released once
bool IsKeyReleased(int key)
{
    if ((key < 0) || (key >= MAX_KEYBOARD_KEYS)) return false;

    return ((CORE.input.keyboard.previousKeyState[key] == 1) && (CORE.input.keyboard.currentKeyState[key] == 0));
}

// Check if a key is NOT being pressed (key not held down)
bool IsKeyUp(int key)
{
    if ((key < 0) || (key >= MAX_KEYBOARD_KEYS)) return false;

    return (CORE.input.keyboard.currentKeyState[key] == 0);
}

// Get the last key pressed
int GetKeyPressed(void)
{
    int value = 0;

    if (CORE.input.keyboard.keyPressedQueueCount > 0)
    {
        // Get character from the queue head
        value = CORE.input.keyboard.keyPressedQueue[0];

        // Shift elements 1 step toward the head
        for (int i = 0; i < (CORE.input.keyboard.keyPressedQueueCount - 1); i++)
        {
            CORE.input.keyboard.keyPressedQueue[i] = CORE.input.keyboard.keyPressedQueue[i + 1];
        }

        // Reset last character in the queue
        CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount - 1] = 0;
        CORE.input.keyboard.keyPressedQueueCount--;
    }

    return value;
}

// Get the last char pressed
int GetCharPressed(void)
{
    int value = 0;

    if (CORE.input.keyboard.charPressedQueueCount > 0)
    {
        // Get character from the queue head
        value = CORE.input.keyboard.charPressedQueue[0];

        // Shift elements 1 step toward the head
        for (int i = 0; i < (CORE.input.keyboard.charPressedQueueCount - 1); i++)
        {
            CORE.input.keyboard.charPressedQueue[i] = CORE.input.keyboard.charPressedQueue[i + 1];
        }

        // Reset last character in the queue
        CORE.input.keyboard.charPressedQueue[CORE.input.keyboard.charPressedQueueCount - 1] = 0;
        CORE.input.keyboard.charPressedQueueCount--;
    }

    return value;
}

// Get name of a QWERTY key on the current keyboard layout
// NOTE: The platform contract exposes no key-name query, so a built-in US layout
// table is used; unknown keys resolve to an empty string (never NULL)
const char *GetKeyName(int key)
{
    static char name[8] = { 0 };
    memset(name, 0, sizeof(name));

    // Printable ASCII range maps 1:1 to raylib keycodes
    if ((key >= KEY_SPACE) && (key <= KEY_GRAVE))
    {
        name[0] = (char)tolower(key);
        return name;
    }

    switch (key)
    {
        case KEY_ESCAPE: return "escape";
        case KEY_ENTER: return "enter";
        case KEY_TAB: return "tab";
        case KEY_BACKSPACE: return "backspace";
        case KEY_INSERT: return "insert";
        case KEY_DELETE: return "delete";
        case KEY_RIGHT: return "right";
        case KEY_LEFT: return "left";
        case KEY_DOWN: return "down";
        case KEY_UP: return "up";
        case KEY_PAGE_UP: return "page up";
        case KEY_PAGE_DOWN: return "page down";
        case KEY_HOME: return "home";
        case KEY_END: return "end";
        case KEY_CAPS_LOCK: return "caps lock";
        case KEY_SCROLL_LOCK: return "scroll lock";
        case KEY_NUM_LOCK: return "num lock";
        case KEY_PRINT_SCREEN: return "print screen";
        case KEY_PAUSE: return "pause";
        case KEY_F1: return "f1";
        case KEY_F2: return "f2";
        case KEY_F3: return "f3";
        case KEY_F4: return "f4";
        case KEY_F5: return "f5";
        case KEY_F6: return "f6";
        case KEY_F7: return "f7";
        case KEY_F8: return "f8";
        case KEY_F9: return "f9";
        case KEY_F10: return "f10";
        case KEY_F11: return "f11";
        case KEY_F12: return "f12";
        case KEY_LEFT_SHIFT: return "left shift";
        case KEY_LEFT_CONTROL: return "left control";
        case KEY_LEFT_ALT: return "left alt";
        case KEY_LEFT_SUPER: return "left super";
        case KEY_RIGHT_SHIFT: return "right shift";
        case KEY_RIGHT_CONTROL: return "right control";
        case KEY_RIGHT_ALT: return "right alt";
        case KEY_RIGHT_SUPER: return "right super";
        case KEY_KB_MENU: return "menu";
        case KEY_KP_0: return "keypad 0";
        case KEY_KP_1: return "keypad 1";
        case KEY_KP_2: return "keypad 2";
        case KEY_KP_3: return "keypad 3";
        case KEY_KP_4: return "keypad 4";
        case KEY_KP_5: return "keypad 5";
        case KEY_KP_6: return "keypad 6";
        case KEY_KP_7: return "keypad 7";
        case KEY_KP_8: return "keypad 8";
        case KEY_KP_9: return "keypad 9";
        case KEY_KP_DECIMAL: return "keypad .";
        case KEY_KP_DIVIDE: return "keypad /";
        case KEY_KP_MULTIPLY: return "keypad *";
        case KEY_KP_SUBTRACT: return "keypad -";
        case KEY_KP_ADD: return "keypad +";
        case KEY_KP_ENTER: return "keypad enter";
        case KEY_KP_EQUAL: return "keypad =";
        default: break;
    }

    return name;    // Empty string
}

// Set a custom key to exit program
// NOTE: default exitKey is set to ESCAPE
void SetExitKey(int key)
{
    if ((key < 0) || (key >= MAX_KEYBOARD_KEYS)) CORE.input.keyboard.exitKey = KEY_NULL;
    else CORE.input.keyboard.exitKey = key;
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Input handling — gamepads
//----------------------------------------------------------------------------------

// Check if a gamepad is available
bool IsGamepadAvailable(int gamepad)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS)) return false;

    return CORE.input.gamepad.ready[gamepad];
}

// Get gamepad internal name id
const char *GetGamepadName(int gamepad)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS) || !CORE.input.gamepad.ready[gamepad]) return "";

    return CORE.input.gamepad.name[gamepad];
}

// Check if a gamepad button has been pressed once
bool IsGamepadButtonPressed(int gamepad, int button)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS) || (button < 0) || (button >= MAX_GAMEPAD_BUTTONS)) return false;

    return ((CORE.input.gamepad.previousButtonState[gamepad][button] == 0) &&
            (CORE.input.gamepad.currentButtonState[gamepad][button] == 1));
}

// Check if a gamepad button is being pressed
bool IsGamepadButtonDown(int gamepad, int button)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS) || (button < 0) || (button >= MAX_GAMEPAD_BUTTONS)) return false;

    return (CORE.input.gamepad.currentButtonState[gamepad][button] == 1);
}

// Check if a gamepad button has been released once
bool IsGamepadButtonReleased(int gamepad, int button)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS) || (button < 0) || (button >= MAX_GAMEPAD_BUTTONS)) return false;

    return ((CORE.input.gamepad.previousButtonState[gamepad][button] == 1) &&
            (CORE.input.gamepad.currentButtonState[gamepad][button] == 0));
}

// Check if a gamepad button is NOT being pressed
bool IsGamepadButtonUp(int gamepad, int button)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS) || (button < 0) || (button >= MAX_GAMEPAD_BUTTONS)) return false;

    return (CORE.input.gamepad.currentButtonState[gamepad][button] == 0);
}

// Get the last gamepad button pressed
int GetGamepadButtonPressed(void)
{
    return CORE.input.gamepad.lastButtonPressed;
}

// Get gamepad axis count
int GetGamepadAxisCount(int gamepad)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS)) return 0;

    return CORE.input.gamepad.axisCount[gamepad];
}

// Get axis movement vector for a gamepad
float GetGamepadAxisMovement(int gamepad, int axis)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS) || (axis < 0) || (axis >= MAX_GAMEPAD_AXES)) return 0.0f;
    if (!CORE.input.gamepad.ready[gamepad]) return 0.0f;

    float value = (axis == GAMEPAD_AXIS_LEFT_TRIGGER) || (axis == GAMEPAD_AXIS_RIGHT_TRIGGER)? -1.0f : 0.0f;

    // Return the raw value only when it is above the dead zone
    float movement = CORE.input.gamepad.axisState[gamepad][axis];
    if ((movement < -0.1f) || (movement > 0.1f)) value = movement;

    return value;
}

// Set internal gamepad mappings
int SetGamepadMappings(const char *mappings)
{
    if (mappings == NULL) return 0;

    return SetGamepadMappingsPlatform(mappings);
}

// Set gamepad vibration for both motors (duration in seconds)
void SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS) || !CORE.input.gamepad.ready[gamepad]) return;

    if (leftMotor < 0.0f) leftMotor = 0.0f;
    if (leftMotor > 1.0f) leftMotor = 1.0f;
    if (rightMotor < 0.0f) rightMotor = 0.0f;
    if (rightMotor > 1.0f) rightMotor = 1.0f;
    if (duration <= 0.0f) return;
    if (duration > MAX_GAMEPAD_VIBRATION_TIME) duration = MAX_GAMEPAD_VIBRATION_TIME;

    SetGamepadVibrationPlatform(gamepad, leftMotor, rightMotor, duration);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Input handling — mouse
//----------------------------------------------------------------------------------

// Check if a mouse button has been pressed once
bool IsMouseButtonPressed(int button)
{
    if ((button < 0) || (button >= MAX_MOUSE_BUTTONS)) return false;

    bool pressed = ((CORE.input.mouse.currentButtonState[button] == 1) && (CORE.input.mouse.previousButtonState[button] == 0));

    // Map touches to mouse buttons checking
    if ((CORE.input.touch.currentTouchState[button] == 1) && (CORE.input.touch.previousTouchState[button] == 0)) pressed = true;

    return pressed;
}

// Check if a mouse button is being pressed
bool IsMouseButtonDown(int button)
{
    if ((button < 0) || (button >= MAX_MOUSE_BUTTONS)) return false;

    bool down = (CORE.input.mouse.currentButtonState[button] == 1);

    // NOTE: Touches are considered like mouse buttons
    if (CORE.input.touch.currentTouchState[button] == 1) down = true;

    return down;
}

// Check if a mouse button has been released once
bool IsMouseButtonReleased(int button)
{
    if ((button < 0) || (button >= MAX_MOUSE_BUTTONS)) return false;

    bool released = ((CORE.input.mouse.currentButtonState[button] == 0) && (CORE.input.mouse.previousButtonState[button] == 1));

    // Map touches to mouse buttons checking
    if ((CORE.input.touch.currentTouchState[button] == 0) && (CORE.input.touch.previousTouchState[button] == 1)) released = true;

    return released;
}

// Check if a mouse button is NOT being pressed
bool IsMouseButtonUp(int button)
{
    return !IsMouseButtonDown(button);
}

// Get mouse position X
int GetMouseX(void)
{
    return (int)((CORE.input.mouse.currentPosition.x + CORE.input.mouse.offset.x)*CORE.input.mouse.scale.x);
}

// Get mouse position Y
int GetMouseY(void)
{
    return (int)((CORE.input.mouse.currentPosition.y + CORE.input.mouse.offset.y)*CORE.input.mouse.scale.y);
}

// Get mouse position XY
Vector2 GetMousePosition(void)
{
    Vector2 position = { 0 };

    position.x = (CORE.input.mouse.currentPosition.x + CORE.input.mouse.offset.x)*CORE.input.mouse.scale.x;
    position.y = (CORE.input.mouse.currentPosition.y + CORE.input.mouse.offset.y)*CORE.input.mouse.scale.y;

    return position;
}

// Get mouse delta between frames
Vector2 GetMouseDelta(void)
{
    Vector2 delta = { 0 };

    delta.x = CORE.input.mouse.currentPosition.x - CORE.input.mouse.previousPosition.x;
    delta.y = CORE.input.mouse.currentPosition.y - CORE.input.mouse.previousPosition.y;

    return delta;
}

// Set mouse position XY
void SetMousePosition(int x, int y)
{
    CORE.input.mouse.currentPosition = (Vector2){ (float)x, (float)y };
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;

    // NOTE: emscripten not supported
    SetMousePositionPlatform(x, y);
}

// Set mouse offset
// NOTE: Useful when rendering to different size targets
void SetMouseOffset(int offsetX, int offsetY)
{
    CORE.input.mouse.offset = (Vector2){ (float)offsetX, (float)offsetY };
}

// Set mouse scaling
// NOTE: Useful when rendering to different size targets
void SetMouseScale(float scaleX, float scaleY)
{
    CORE.input.mouse.scale = (Vector2){ scaleX, scaleY };
}

// Get mouse wheel movement Y
float GetMouseWheelMove(void)
{
    float result = 0.0f;

    if (fabsf(CORE.input.mouse.currentWheelMove.x) > fabsf(CORE.input.mouse.currentWheelMove.y)) result = (float)CORE.input.mouse.currentWheelMove.x;
    else result = (float)CORE.input.mouse.currentWheelMove.y;

    return result;
}

// Get mouse wheel movement X/Y as a vector
Vector2 GetMouseWheelMoveV(void)
{
    return CORE.input.mouse.currentWheelMove;
}

// Set mouse cursor
void SetMouseCursor(int cursor)
{
    CORE.input.mouse.cursor = cursor;
    SetMouseCursorPlatform(cursor);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Input handling — touch
//----------------------------------------------------------------------------------

// Get touch position X for touch point 0 (relative to screen size)
int GetTouchX(void)
{
    return (int)CORE.input.touch.position[0].x;
}

// Get touch position Y for touch point 0 (relative to screen size)
int GetTouchY(void)
{
    return (int)CORE.input.touch.position[0].y;
}

// Get touch position XY for a touch point index (relative to screen size)
Vector2 GetTouchPosition(int index)
{
    Vector2 position = { -1.0f, -1.0f };

    if ((index >= 0) && (index < MAX_TOUCH_POINTS)) position = CORE.input.touch.position[index];
    else TRACELOG(LOG_WARNING, "INPUT: Required touch point out of range (Max touch points: %i)", MAX_TOUCH_POINTS);

    return position;
}

// Get touch point identifier for given index
int GetTouchPointId(int index)
{
    int id = -1;

    if ((index >= 0) && (index < MAX_TOUCH_POINTS)) id = CORE.input.touch.pointId[index];

    return id;
}

// Get number of touch points
int GetTouchPointCount(void)
{
    return CORE.input.touch.pointCount;
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Input events processing
//----------------------------------------------------------------------------------

// Register all input events
// NOTE: Rotates previous/current input state, then lets the platform backend fill
// the current state from its native event pump
void PollInputEvents(void)
{
#if SUPPORT_GESTURES_SYSTEM
    // NOTE: Gestures update must be called every frame to reset gestures correctly
    // because ProcessGestureEvent() is just called on an event, not every frame
    UpdateGestures();
#endif

    // Reset keys/chars pressed registered
    CORE.input.keyboard.keyPressedQueueCount = 0;
    CORE.input.keyboard.charPressedQueueCount = 0;

    // Reset last gamepad button/axis registered state
    CORE.input.gamepad.lastButtonPressed = GAMEPAD_BUTTON_UNKNOWN;

    // Register previous keys states
    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++)
    {
        CORE.input.keyboard.previousKeyState[i] = CORE.input.keyboard.currentKeyState[i];
        CORE.input.keyboard.keyRepeatInFrame[i] = 0;
    }

    // Register previous mouse states
    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) CORE.input.mouse.previousButtonState[i] = CORE.input.mouse.currentButtonState[i];

    // Register previous mouse wheel state
    CORE.input.mouse.previousWheelMove = CORE.input.mouse.currentWheelMove;
    CORE.input.mouse.currentWheelMove = (Vector2){ 0.0f, 0.0f };

    // Register previous mouse position
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;

    // Register previous touch states
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.input.touch.previousTouchState[i] = CORE.input.touch.currentTouchState[i];

    // Register previous gamepad button states
    for (int i = 0; i < MAX_GAMEPADS; i++)
    {
        if (!CORE.input.gamepad.ready[i]) continue;

        for (int k = 0; k < MAX_GAMEPAD_BUTTONS; k++) CORE.input.gamepad.previousButtonState[i][k] = CORE.input.gamepad.currentButtonState[i][k];
    }

    // Reset window resize state, it is set by the platform on resize events
    CORE.window.resizedLastFrame = false;

    // Let the platform backend pump native events into CORE
    PollInputEventsPlatform();

#if SUPPORT_GESTURES_SYSTEM
    UpdateGestureEvents();
#endif
}

#if SUPPORT_GESTURES_SYSTEM
// Feed the gestures system from the current touch (or emulated mouse) state
// NOTE: raylib feeds gesture events from platform callbacks; here the state is
// sampled once per frame, which keeps drag/pinch math identical but limits
// gesture resolution to the frame rate
static void UpdateGestureEvents(void)
{
    GestureEvent gestureEvent = { 0 };

    int pointCount = CORE.input.touch.pointCount;
    if (pointCount > MAX_TOUCH_POINTS) pointCount = MAX_TOUCH_POINTS;

    bool current[MAX_TOUCH_POINTS] = { 0 };
    bool previous[MAX_TOUCH_POINTS] = { 0 };

    if (pointCount > 0)
    {
        for (int i = 0; i < pointCount; i++)
        {
            gestureEvent.pointId[i] = CORE.input.touch.pointId[i];
            gestureEvent.position[i] = CORE.input.touch.position[i];
            current[i] = (CORE.input.touch.currentTouchState[i] == 1);
            previous[i] = (CORE.input.touch.previousTouchState[i] == 1);
        }
    }
#if SUPPORT_MOUSE_GESTURES
    else
    {
        // Emulate a single touch point from the left mouse button
        bool down = (CORE.input.mouse.currentButtonState[MOUSE_BUTTON_LEFT] == 1);
        bool wasDown = (CORE.input.mouse.previousButtonState[MOUSE_BUTTON_LEFT] == 1);

        if (!down && !wasDown) return;      // Nothing to report

        pointCount = 1;
        gestureEvent.pointId[0] = 0;
        gestureEvent.position[0] = GetMousePosition();
        current[0] = down;
        previous[0] = wasDown;
    }
#endif

    if (pointCount == 0) return;

    // Normalize gesture positions to screen size, as the gestures system expects
    for (int i = 0; i < pointCount; i++)
    {
        gestureEvent.position[i].x /= (float)GetScreenWidth();
        gestureEvent.position[i].y /= (float)GetScreenHeight();
    }

    bool anyDownEdge = false;
    bool anyUpEdge = false;
    bool anyDown = false;

    for (int i = 0; i < pointCount; i++)
    {
        if (current[i] && !previous[i]) anyDownEdge = true;
        if (!current[i] && previous[i]) anyUpEdge = true;
        if (current[i]) anyDown = true;
    }

    if (anyDownEdge) gestureEvent.touchAction = TOUCH_ACTION_DOWN;
    else if (anyUpEdge) gestureEvent.touchAction = TOUCH_ACTION_UP;
    else if (anyDown) gestureEvent.touchAction = TOUCH_ACTION_MOVE;
    else return;    // No active or transitioning points

    gestureEvent.pointCount = pointCount;

    ProcessGestureEvent(gestureEvent);
}
#endif  // SUPPORT_GESTURES_SYSTEM

//----------------------------------------------------------------------------------
// Module Functions Definition: Compression and encoding
//----------------------------------------------------------------------------------

// Compress data (DEFLATE algorithm)
unsigned char *CompressData(const unsigned char *data, int dataSize, int *compDataSize)
{
    #define COMPRESSION_QUALITY_DEFLATE  8

    unsigned char *compData = NULL;
    if (compDataSize != NULL) *compDataSize = 0;

    if ((data == NULL) || (dataSize <= 0)) return NULL;

#if SUPPORT_COMPRESSION_API
    // Compress data and generate a valid DEFLATE stream
    // NOTE: struct sdefl is ~1MB, it must not be allocated on the stack
    struct sdefl *sdeflComp = (struct sdefl *)RL_CALLOC(1, sizeof(struct sdefl));
    if (sdeflComp == NULL) return NULL;

    int bounds = sdefl_bound(dataSize);
    compData = (unsigned char *)RL_CALLOC(bounds, 1);

    if (compData != NULL)
    {
        int size = sdeflate(sdeflComp, compData, data, dataSize, COMPRESSION_QUALITY_DEFLATE);
        if (compDataSize != NULL) *compDataSize = size;

        TRACELOG(LOG_INFO, "SYSTEM: Compress data: Original size: %i -> Comp. size: %i", dataSize, size);
    }

    RL_FREE(sdeflComp);
#else
    TRACELOG(LOG_WARNING, "SYSTEM: Compression API not enabled (SUPPORT_COMPRESSION_API)");
#endif

    return compData;
}

// Decompress data (DEFLATE algorithm), internal entry point shared with other modules
unsigned char *DecompressDataInternal(const unsigned char *compData, int compDataSize, int *dataSize)
{
    unsigned char *data = NULL;
    if (dataSize != NULL) *dataSize = 0;

    if ((compData == NULL) || (compDataSize <= 0)) return NULL;

#if SUPPORT_COMPRESSION_API
    // Decompress data from a valid DEFLATE stream
    data = (unsigned char *)RL_CALLOC(MAX_DECOMPRESSION_SIZE*1024*1024, 1);
    if (data == NULL) return NULL;

    int length = sinflate(data, MAX_DECOMPRESSION_SIZE*1024*1024, compData, compDataSize);

    if (length < 0)
    {
        TRACELOG(LOG_WARNING, "SYSTEM: Failed to decompress data");
        RL_FREE(data);
        return NULL;
    }

    // WARNING: RL_REALLOC can lose the pointer if the reallocation fails
    unsigned char *temp = (unsigned char *)RL_REALLOC(data, length);
    if (temp != NULL) data = temp;
    else TRACELOG(LOG_WARNING, "SYSTEM: Failed to re-allocate required decompression memory");

    if (dataSize != NULL) *dataSize = length;

    TRACELOG(LOG_INFO, "SYSTEM: Decompress data: Comp. size: %i -> Original size: %i", compDataSize, length);
#else
    TRACELOG(LOG_WARNING, "SYSTEM: Compression API not enabled (SUPPORT_COMPRESSION_API)");
#endif

    return data;
}

// Decompress data (DEFLATE algorithm)
unsigned char *DecompressData(const unsigned char *compData, int compDataSize, int *dataSize)
{
    return DecompressDataInternal(compData, compDataSize, dataSize);
}

// Encode data to Base64 string, memory must be MemFree()
char *EncodeDataBase64(const unsigned char *data, int dataSize, int *outputSize)
{
    static const unsigned char base64encodeTable[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'
    };

    static const int modTable[] = { 0, 2, 1 };

    if (outputSize != NULL) *outputSize = 0;
    if ((data == NULL) || (dataSize <= 0)) return NULL;

    int outSize = 4*((dataSize + 2)/3);
    char *encodedData = (char *)RL_MALLOC(outSize + 1);
    if (encodedData == NULL) return NULL;

    for (int i = 0, j = 0; i < dataSize;)
    {
        unsigned int octetA = (i < dataSize)? (unsigned char)data[i++] : 0;
        unsigned int octetB = (i < dataSize)? (unsigned char)data[i++] : 0;
        unsigned int octetC = (i < dataSize)? (unsigned char)data[i++] : 0;

        unsigned int triple = (octetA << 0x10) + (octetB << 0x08) + octetC;

        encodedData[j++] = base64encodeTable[(triple >> 3*6) & 0x3F];
        encodedData[j++] = base64encodeTable[(triple >> 2*6) & 0x3F];
        encodedData[j++] = base64encodeTable[(triple >> 1*6) & 0x3F];
        encodedData[j++] = base64encodeTable[(triple >> 0*6) & 0x3F];
    }

    for (int i = 0; i < modTable[dataSize%3]; i++) encodedData[outSize - 1 - i] = '=';  // Padding character

    encodedData[outSize] = '\0';    // Add NULL terminator

    if (outputSize != NULL) *outputSize = outSize + 1;

    return encodedData;
}

// Decode Base64 string (expected NULL terminated), memory must be MemFree()
unsigned char *DecodeDataBase64(const char *text, int *outputSize)
{
    static const unsigned char base64decodeTable[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0, 63, 52, 53, 54,
        55, 56, 57, 58, 59, 60, 61, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
        10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0, 0,
        0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
        45, 46, 47, 48, 49, 50, 51
    };

    if (outputSize != NULL) *outputSize = 0;
    if (text == NULL) return NULL;

    // Base64 streams are always a multiple of 4 characters
    size_t textLength = strlen(text);
    if ((textLength == 0) || ((textLength%4) != 0))
    {
        TRACELOG(LOG_WARNING, "SYSTEM: Invalid Base64 input data length: %i", (int)textLength);
        return NULL;
    }

    // Get output size of Base64 input data
    int outSize = 0;
    for (int i = 0; text[4*i] != '\0'; i++)
    {
        if (text[4*i + 3] == '=')
        {
            if (text[4*i + 2] == '=') outSize += 1;
            else outSize += 2;
        }
        else outSize += 3;
    }

    if (outSize <= 0) return NULL;

    // Allocate memory to store decoded Base64 data
    unsigned char *decodedData = (unsigned char *)RL_MALLOC(outSize);
    if (decodedData == NULL) return NULL;

    for (int i = 0; i < outSize/3; i++)
    {
        unsigned char a = base64decodeTable[(unsigned char)text[4*i]];
        unsigned char b = base64decodeTable[(unsigned char)text[4*i + 1]];
        unsigned char c = base64decodeTable[(unsigned char)text[4*i + 2]];
        unsigned char d = base64decodeTable[(unsigned char)text[4*i + 3]];

        decodedData[3*i] = (a << 2) | (b >> 4);
        decodedData[3*i + 1] = (b << 4) | (c >> 2);
        decodedData[3*i + 2] = (c << 6) | d;
    }

    if ((outSize%3) == 1)
    {
        int n = outSize/3;
        unsigned char a = base64decodeTable[(unsigned char)text[4*n]];
        unsigned char b = base64decodeTable[(unsigned char)text[4*n + 1]];
        decodedData[outSize - 1] = (a << 2) | (b >> 4);
    }
    else if ((outSize%3) == 2)
    {
        int n = outSize/3;
        unsigned char a = base64decodeTable[(unsigned char)text[4*n]];
        unsigned char b = base64decodeTable[(unsigned char)text[4*n + 1]];
        unsigned char c = base64decodeTable[(unsigned char)text[4*n + 2]];
        decodedData[outSize - 2] = (a << 2) | (b >> 4);
        decodedData[outSize - 1] = (b << 4) | (c >> 2);
    }

    if (outputSize != NULL) *outputSize = outSize;

    return decodedData;
}

// Compute CRC32 hash code
unsigned int ComputeCRC32(const unsigned char *data, int dataSize)
{
    static const unsigned int crcTable[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
        0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
        0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
        0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
        0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
        0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
        0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
        0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
        0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
        0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
        0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
        0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
        0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
        0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
        0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
        0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
        0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
        0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
        0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
        0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
        0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
        0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
        0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
        0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
        0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
        0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
        0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
        0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
        0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
        0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
        0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
    };

    unsigned int crc = ~0u;

    if ((data == NULL) || (dataSize <= 0)) return 0;

    for (int i = 0; i < dataSize; i++) crc = (crc >> 8)^crcTable[data[i]^(crc & 0xff)];

    return ~crc;
}

// Compute MD5 hash code, returns static int[4] (16 bytes)
// NOTE: Reference implementation: http://www.zedwood.com/article/cpp-md5-function
unsigned int *ComputeMD5(const unsigned char *data, int dataSize)
{
    #define LEFTROTATE(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

    static unsigned int hash[4] = { 0 };

    // NOTE: Every value is reset on every call
    hash[0] = 0x67452301;
    hash[1] = 0xefcdab89;
    hash[2] = 0x98badcfe;
    hash[3] = 0x10325476;

    if ((data == NULL) || (dataSize < 0)) return hash;

    static const unsigned int k[] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };

    static const int r[] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };

    // Pre-processing: adding a single 1 bit
    // Append '1' bit to message
    // NOTE: The input bytes are considered as bits strings,
    // where the first bit is the most significant bit of the byte

    // Pre-processing: padding with zeros
    // Append 0 bit until message length in bit 448 (mod 512)
    // Append length mod (2 pow 64) to message

    int newDataSize = ((((dataSize + 8)/64) + 1)*64) - 8;

    unsigned char *msg = (unsigned char *)RL_CALLOC(newDataSize + 64, 1);   // Also appends "0" bits (we alloc also 64 extra bytes...)
    if (msg == NULL) return hash;

    memcpy(msg, data, dataSize);
    msg[dataSize] = 128;                    // Write the "1" bit

    unsigned int bitsLen = 8*(unsigned int)dataSize;
    memcpy(msg + newDataSize, &bitsLen, 4); // We append the len in bits at the end of the buffer

    // Process the message in successive 512-bit chunks for each 512-bit chunk of message
    for (int offset = 0; offset < newDataSize; offset += (512/8))
    {
        // Break chunk into sixteen 32-bit words w[j], 0 <= j <= 15
        unsigned int w[16] = { 0 };
        for (int i = 0; i < 16; i++) memcpy(w + i, msg + offset + i*4, 4);

        // Initialize hash value for this chunk
        unsigned int a = hash[0];
        unsigned int b = hash[1];
        unsigned int c = hash[2];
        unsigned int d = hash[3];

        for (int i = 0; i < 64; i++)
        {
            unsigned int f = 0;
            unsigned int g = 0;

            if (i < 16)
            {
                f = (b & c) | ((~b) & d);
                g = i;
            }
            else if (i < 32)
            {
                f = (d & b) | ((~d) & c);
                g = (5*i + 1)%16;
            }
            else if (i < 48)
            {
                f = b^c^d;
                g = (3*i + 5)%16;
            }
            else
            {
                f = c^(b | (~d));
                g = (7*i)%16;
            }

            unsigned int temp = d;
            d = c;
            c = b;
            b = b + LEFTROTATE((a + f + k[i] + w[g]), r[i]);
            a = temp;
        }

        // Add this chunk's hash to result so far
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
    }

    RL_FREE(msg);

    return hash;
}

// Compute SHA-1 hash code, returns static int[5] (20 bytes)
// NOTE: Reference implementation: https://github.com/clibs/sha1
unsigned int *ComputeSHA1(const unsigned char *data, int dataSize)
{
    #define ROTLEFT(a, b) (((a) << (b)) | ((a) >> (32 - (b))))

    static unsigned int hash[5] = { 0 };    // Fixed 160 bit hash size

    hash[0] = 0x67452301;
    hash[1] = 0xEFCDAB89;
    hash[2] = 0x98BADCFE;
    hash[3] = 0x10325476;
    hash[4] = 0xC3D2E1F0;

    if ((data == NULL) || (dataSize < 0)) return hash;

    // Pre-processing: padding
    int newDataSize = ((((dataSize + 8)/64) + 1)*64);

    unsigned char *msg = (unsigned char *)RL_CALLOC(newDataSize, 1);
    if (msg == NULL) return hash;

    memcpy(msg, data, dataSize);
    msg[dataSize] = 0x80;

    unsigned long long bitsLen = 8ull*(unsigned long long)dataSize;
    for (int i = 0; i < 8; i++) msg[newDataSize - 1 - i] = (unsigned char)((bitsLen >> (8*i)) & 0xff);

    for (int offset = 0; offset < newDataSize; offset += 64)
    {
        unsigned int w[80] = { 0 };

        for (int i = 0; i < 16; i++)
        {
            w[i] = ((unsigned int)msg[offset + i*4] << 24) | ((unsigned int)msg[offset + i*4 + 1] << 16) |
                   ((unsigned int)msg[offset + i*4 + 2] << 8) | ((unsigned int)msg[offset + i*4 + 3]);
        }

        for (int i = 16; i < 80; i++) w[i] = ROTLEFT(w[i - 3]^w[i - 8]^w[i - 14]^w[i - 16], 1);

        unsigned int a = hash[0];
        unsigned int b = hash[1];
        unsigned int c = hash[2];
        unsigned int d = hash[3];
        unsigned int e = hash[4];

        for (int i = 0; i < 80; i++)
        {
            unsigned int f = 0;
            unsigned int k = 0;

            if (i < 20)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            }
            else if (i < 40)
            {
                f = b^c^d;
                k = 0x6ED9EBA1;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            }
            else
            {
                f = b^c^d;
                k = 0xCA62C1D6;
            }

            unsigned int temp = ROTLEFT(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = ROTLEFT(b, 30);
            b = a;
            a = temp;
        }

        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
    }

    RL_FREE(msg);

    return hash;
}

// Compute SHA-256 hash code, returns static int[8] (32 bytes)
// NOTE: Reference implementation: FIPS 180-4
unsigned int *ComputeSHA256(const unsigned char *data, int dataSize)
{
    #define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    #define SIG0(x) (ROTR((x), 7)^ROTR((x), 18)^((x) >> 3))
    #define SIG1(x) (ROTR((x), 17)^ROTR((x), 19)^((x) >> 10))
    #define EP0(x)  (ROTR((x), 2)^ROTR((x), 13)^ROTR((x), 22))
    #define EP1(x)  (ROTR((x), 6)^ROTR((x), 11)^ROTR((x), 25))
    #define CH(x, y, z)  (((x) & (y))^((~(x)) & (z)))
    #define MAJ(x, y, z) (((x) & (y))^((x) & (z))^((y) & (z)))

    static const unsigned int k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    static unsigned int hash[8] = { 0 };    // Fixed 256 bit hash size

    hash[0] = 0x6a09e667;
    hash[1] = 0xbb67ae85;
    hash[2] = 0x3c6ef372;
    hash[3] = 0xa54ff53a;
    hash[4] = 0x510e527f;
    hash[5] = 0x9b05688c;
    hash[6] = 0x1f83d9ab;
    hash[7] = 0x5be0cd19;

    if ((data == NULL) || (dataSize < 0)) return hash;

    // Pre-processing: padding
    int newDataSize = ((((dataSize + 8)/64) + 1)*64);

    unsigned char *msg = (unsigned char *)RL_CALLOC(newDataSize, 1);
    if (msg == NULL) return hash;

    memcpy(msg, data, dataSize);
    msg[dataSize] = 0x80;

    unsigned long long bitsLen = 8ull*(unsigned long long)dataSize;
    for (int i = 0; i < 8; i++) msg[newDataSize - 1 - i] = (unsigned char)((bitsLen >> (8*i)) & 0xff);

    for (int offset = 0; offset < newDataSize; offset += 64)
    {
        unsigned int w[64] = { 0 };

        for (int i = 0; i < 16; i++)
        {
            w[i] = ((unsigned int)msg[offset + i*4] << 24) | ((unsigned int)msg[offset + i*4 + 1] << 16) |
                   ((unsigned int)msg[offset + i*4 + 2] << 8) | ((unsigned int)msg[offset + i*4 + 3]);
        }

        for (int i = 16; i < 64; i++) w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];

        unsigned int a = hash[0];
        unsigned int b = hash[1];
        unsigned int c = hash[2];
        unsigned int d = hash[3];
        unsigned int e = hash[4];
        unsigned int f = hash[5];
        unsigned int g = hash[6];
        unsigned int h = hash[7];

        for (int i = 0; i < 64; i++)
        {
            unsigned int t1 = h + EP1(e) + CH(e, f, g) + k[i] + w[i];
            unsigned int t2 = EP0(a) + MAJ(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    RL_FREE(msg);

    return hash;
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Files management
//----------------------------------------------------------------------------------

#if defined(_WIN32)
    #define MKDIR(dir) _mkdir(dir)
    // NOTE: Avoid including windows.h just for one symbol
    unsigned int __stdcall GetModuleFileNameA(void *hModule, void *lpFilename, unsigned int nSize);
#else
    #define MKDIR(dir) mkdir(dir, 0777)
#endif

#define DIRECTORY_FILTER_TAG    "DIR"       // Name tag used to request directory inclusion on directory scan

// Rename file (if it exists)
int FileRename(const char *fileName, const char *fileRename)
{
    if ((fileName == NULL) || (fileRename == NULL)) return -1;

    if (!FileExists(fileName))
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] File does not exist", fileName);
        return -1;
    }

    int result = rename(fileName, fileRename);
    if (result != 0) TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to rename file to: %s", fileName, fileRename);

    return result;
}

// Remove file (if it exists)
int FileRemove(const char *fileName)
{
    if (fileName == NULL) return -1;

    if (!FileExists(fileName))
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] File does not exist", fileName);
        return -1;
    }

    int result = remove(fileName);
    if (result != 0) TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to remove file", fileName);

    return result;
}

// Copy file from one path to another, dstPath directories are created if required
int FileCopy(const char *srcPath, const char *dstPath)
{
    if ((srcPath == NULL) || (dstPath == NULL)) return -1;

    int dataSize = 0;
    unsigned char *data = LoadFileData(srcPath, &dataSize);
    if (data == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to read source file to copy", srcPath);
        return -1;
    }

    // Create the destination directory tree if required
    const char *dstDir = GetDirectoryPath(dstPath);
    if ((dstDir != NULL) && (dstDir[0] != '\0') && !DirectoryExists(dstDir)) MakeDirectory(dstDir);

    bool success = SaveFileData(dstPath, data, dataSize);

    UnloadFileData(data);

    if (!success)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to write destination file", dstPath);
        return -1;
    }

    return 0;
}

// Move file from one directory to another, dstPath directories are created if required
int FileMove(const char *srcPath, const char *dstPath)
{
    if ((srcPath == NULL) || (dstPath == NULL)) return -1;

    if (!FileExists(srcPath))
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] File does not exist", srcPath);
        return -1;
    }

    // Try the cheap path first, it only works within the same filesystem
    const char *dstDir = GetDirectoryPath(dstPath);
    if ((dstDir != NULL) && (dstDir[0] != '\0') && !DirectoryExists(dstDir)) MakeDirectory(dstDir);

    if (rename(srcPath, dstPath) == 0) return 0;

    // Fallback: copy + remove
    if (FileCopy(srcPath, dstPath) != 0) return -1;

    return FileRemove(srcPath);
}

// Replace text in an existing file
int FileTextReplace(const char *fileName, const char *search, const char *replacement)
{
    if ((fileName == NULL) || (search == NULL) || (replacement == NULL)) return -1;

    char *fileText = LoadFileText(fileName);
    if (fileText == NULL) return -1;

    int result = 0;
    char *fileTextUpdated = TextReplaceAlloc(fileText, search, replacement);

    if (fileTextUpdated != NULL)
    {
        if (!SaveFileText(fileName, fileTextUpdated)) result = -1;
        MemFree(fileTextUpdated);
    }
    else result = -1;

    UnloadFileText(fileText);

    return result;
}

// Find text in an existing file, returns -1 if not found or the byte index otherwise
int FileTextFindIndex(const char *fileName, const char *search)
{
    if ((fileName == NULL) || (search == NULL)) return -1;

    char *fileText = LoadFileText(fileName);
    if (fileText == NULL) return -1;

    int index = -1;
    const char *found = strstr(fileText, search);
    if (found != NULL) index = (int)(found - fileText);

    UnloadFileText(fileText);

    return index;
}

// Check if the file exists
bool FileExists(const char *fileName)
{
    if (fileName == NULL) return false;

    struct stat info = { 0 };
    if (stat(fileName, &info) != 0) return false;

    return S_ISREG(info.st_mode);
}

// Check if a directory path exists
bool DirectoryExists(const char *dirPath)
{
    if (dirPath == NULL) return false;

    struct stat info = { 0 };
    if (stat(dirPath, &info) != 0) return false;

    return S_ISDIR(info.st_mode);
}

// Check file extension
// NOTE: Extensions checking is not case-sensitive, ext can contain several ones separated by ';'
bool IsFileExtension(const char *fileName, const char *ext)
{
    if ((fileName == NULL) || (ext == NULL)) return false;

    const char *fileExt = GetFileExtension(fileName);
    if (fileExt == NULL) return false;

    // Check the requested extensions one by one (separated by ';')
    const char *cursor = ext;
    while (*cursor != '\0')
    {
        const char *sep = strchr(cursor, ';');
        size_t len = (sep != NULL)? (size_t)(sep - cursor) : strlen(cursor);

        if ((len > 0) && (len == strlen(fileExt)))
        {
            size_t i = 0;
            for (i = 0; i < len; i++)
            {
                if (tolower((unsigned char)cursor[i]) != tolower((unsigned char)fileExt[i])) break;
            }

            if (i == len) return true;
        }

        if (sep == NULL) break;
        cursor = sep + 1;
    }

    return false;
}

// Get file length in bytes
// NOTE: GetFileSize() conflicts with windows.h
int GetFileLength(const char *fileName)
{
    int size = 0;

    if (fileName == NULL) return 0;

    // NOTE: On Unix-like systems, it can return size for a directory or a device
    if (!FileExists(fileName)) return 0;

    FILE *file = fopen(fileName, "rb");
    if (file == NULL) return 0;

    fseek(file, 0L, SEEK_END);
    long fileSize = ftell(file);
    fclose(file);

    // Check for size overflow (INT_MAX)
    if (fileSize > 2147483647L) TRACELOG(LOG_WARNING, "FILEIO: [%s] File size overflows expected limit, do not use SaveFileData()", fileName);
    else size = (int)fileSize;

    return size;
}

// Get pointer to extension for a filename string (includes the dot: .png)
const char *GetFileExtension(const char *fileName)
{
    if (fileName == NULL) return NULL;

    const char *dot = strrchr(fileName, '.');
    if ((dot == NULL) || (dot == fileName)) return NULL;

    return dot;
}

// Get pointer to filename for a path string
const char *GetFileName(const char *filePath)
{
    if (filePath == NULL) return NULL;

    const char *fileName = NULL;
    for (const char *i = filePath; *i != '\0'; i++)
    {
        if ((*i == '\\') || (*i == '/')) fileName = i;
    }

    if (fileName == NULL) return filePath;

    return fileName + 1;
}

// Get filename string without extension (uses static string)
const char *GetFileNameWithoutExt(const char *filePath)
{
    #define MAX_FILENAMEWITHOUTEXT_LENGTH   256

    static char fileName[MAX_FILENAMEWITHOUTEXT_LENGTH] = { 0 };
    memset(fileName, 0, MAX_FILENAMEWITHOUTEXT_LENGTH);

    if (filePath == NULL) return fileName;

    strncpy(fileName, GetFileName(filePath), MAX_FILENAMEWITHOUTEXT_LENGTH - 1);

    char *dot = strrchr(fileName, '.');
    if ((dot != NULL) && (dot != fileName)) *dot = '\0';

    return fileName;
}

// Get directory for a given filePath (uses static string)
const char *GetDirectoryPath(const char *filePath)
{
    // NOTE: Directory separator is expected to be '/' or '\\'
    const char *lastSlash = NULL;
    static char dirPath[MAX_FILEPATH_LENGTH] = { 0 };
    memset(dirPath, 0, MAX_FILEPATH_LENGTH);

    if (filePath == NULL) return dirPath;

    lastSlash = strrchr(filePath, '/');
    const char *lastBackSlash = strrchr(filePath, '\\');
    if ((lastBackSlash != NULL) && ((lastSlash == NULL) || (lastBackSlash > lastSlash))) lastSlash = lastBackSlash;

    if (lastSlash == NULL)
    {
        // Drive-relative path with no separator: "C:file.txt" -> "C:"
        if ((filePath[0] != '\0') && (filePath[1] == ':'))
        {
            dirPath[0] = filePath[0];
            dirPath[1] = ':';
        }

        return dirPath;
    }

    size_t length = (size_t)(lastSlash - filePath);
    if (length == 0) length = 1;    // Root directory: "/"
    if (length > (MAX_FILEPATH_LENGTH - 1)) length = MAX_FILEPATH_LENGTH - 1;

    memcpy(dirPath, filePath, length);
    dirPath[length] = '\0';

    return dirPath;
}

// Get previous directory path for a given path (uses static string)
const char *GetPrevDirectoryPath(const char *dirPath)
{
    static char prevDirPath[MAX_FILEPATH_LENGTH] = { 0 };
    memset(prevDirPath, 0, MAX_FILEPATH_LENGTH);

    if (dirPath == NULL) return prevDirPath;

    int pathLen = (int)strlen(dirPath);
    if (pathLen <= 3) return dirPath;   // Path is a root directory ("/", "C:\")

    for (int i = (pathLen - 1); (i >= 0) && (pathLen > 3); i--)
    {
        if ((dirPath[i] == '\\') || (dirPath[i] == '/'))
        {
            // Check for root: "C:\" or "/"
            if (((i == 2) && (dirPath[1] == ':')) || (i == 0)) i++;

            strncpy(prevDirPath, dirPath, i);
            prevDirPath[i] = '\0';
            break;
        }
    }

    return prevDirPath;
}

// Get current working directory (uses static string)
const char *GetWorkingDirectory(void)
{
    static char currentDir[MAX_FILEPATH_LENGTH] = { 0 };
    memset(currentDir, 0, MAX_FILEPATH_LENGTH);

    char *path = GETCWD(currentDir, MAX_FILEPATH_LENGTH - 1);
    if (path == NULL) return "";

    return currentDir;
}

// Get the directory of the running application (uses static string)
const char *GetApplicationDirectory(void)
{
    static char appDir[MAX_FILEPATH_LENGTH] = { 0 };
    memset(appDir, 0, MAX_FILEPATH_LENGTH);

#if defined(_WIN32)
    int len = (int)GetModuleFileNameA(NULL, appDir, MAX_FILEPATH_LENGTH - 1);
#elif defined(__linux__)
    int len = (int)readlink("/proc/self/exe", appDir, MAX_FILEPATH_LENGTH - 1);
    if (len < 0) len = 0;
    appDir[len] = '\0';
#elif defined(__APPLE__)
    uint32_t bufferSize = MAX_FILEPATH_LENGTH - 1;
    int len = 0;
    if (_NSGetExecutablePath(appDir, &bufferSize) == 0) len = (int)strlen(appDir);
#else
    int len = 0;
#endif

    if (len <= 0)
    {
        // Fall back to the current working directory
        appDir[0] = '.';
        appDir[1] = '/';
        appDir[2] = '\0';
        return appDir;
    }

    // Strip the executable name, keeping the trailing separator
    for (int i = len - 1; i >= 0; i--)
    {
        if ((appDir[i] == '\\') || (appDir[i] == '/'))
        {
            appDir[i + 1] = '\0';
            return appDir;
        }
    }

    appDir[0] = '.';
    appDir[1] = '/';
    appDir[2] = '\0';

    return appDir;
}

// Create directories (including full path requested), returns 0 on success
int MakeDirectory(const char *dirPath)
{
    if ((dirPath == NULL) || (dirPath[0] == '\0')) return 1;
    if (DirectoryExists(dirPath)) return 0;

    char temp[MAX_FILEPATH_LENGTH] = { 0 };
    strncpy(temp, dirPath, MAX_FILEPATH_LENGTH - 1);

    // Remove any trailing separator
    size_t length = strlen(temp);
    while ((length > 1) && ((temp[length - 1] == '/') || (temp[length - 1] == '\\')))
    {
        temp[length - 1] = '\0';
        length--;
    }

    // Create every intermediate directory of the path
    for (char *p = temp + 1; *p != '\0'; p++)
    {
        if ((*p != '/') && (*p != '\\')) continue;

        char separator = *p;
        *p = '\0';

        if (!DirectoryExists(temp) && (MKDIR(temp) != 0) && (errno != EEXIST))
        {
            TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to create directory", temp);
            return 1;
        }

        *p = separator;
    }

    if (!DirectoryExists(temp) && (MKDIR(temp) != 0) && (errno != EEXIST))
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to create directory", temp);
        return 1;
    }

    return 0;
}

// Change working directory, returns 0 on success
int ChangeDirectory(const char *dirPath)
{
    if (dirPath == NULL) return -1;

    int result = CHDIR(dirPath);
    if (result != 0) TRACELOG(LOG_WARNING, "SYSTEM: Failed to change to directory: %s", dirPath);

    return result;
}

// Check if a given path point to a file
bool IsPathFile(const char *path)
{
    return FileExists(path);
}

// Check if a given path point to a directory
bool IsPathDirectory(const char *path)
{
    return DirectoryExists(path);
}

// Check if a given path is absolute
bool IsPathAbsolute(const char *path)
{
    if ((path == NULL) || (path[0] == '\0')) return false;

    if ((path[0] == '/') || (path[0] == '\\')) return true;

    // Windows drive letter: "C:\" or "c:/"
    if ((path[1] == ':') && ((path[2] == '\\') || (path[2] == '/'))) return true;

    return false;
}

// Check if fileName is valid for the platform/OS
bool IsFileNameValid(const char *fileName)
{
    bool valid = true;

    if ((fileName == NULL) || (fileName[0] == '\0')) return false;

    int length = (int)strlen(fileName);
    if (length > 255) return false;     // Common maximum on most filesystems

    // Check invalid characters and control codes
    for (int i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char)fileName[i];

        if (c < 32) { valid = false; break; }

        if ((c == '<') || (c == '>') || (c == ':') || (c == '"') ||
            (c == '/') || (c == '\\') || (c == '|') || (c == '?') || (c == '*')) { valid = false; break; }
    }

    // Trailing dots and spaces are not allowed on all platforms
    if (valid && ((fileName[length - 1] == '.') || (fileName[length - 1] == ' '))) valid = false;

    return valid;
}

// Load directory filepaths
// NOTE: Files count is returned by parameter, memory should be freed with UnloadDirectoryFiles()
FilePathList LoadDirectoryFiles(const char *dirPath)
{
    FilePathList files = { 0 };
    unsigned int fileCounter = 0;

    if (dirPath == NULL) return files;

    struct dirent *entity = NULL;
    DIR *dir = opendir(dirPath);

    if (dir == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: Failed to open requested directory: %s", dirPath);
        return files;
    }

    // SCAN 1: Count files
    while ((entity = readdir(dir)) != NULL)
    {
        // NOTE: We skip '.' (current dir) and '..' (parent dir) filepaths
        if ((strcmp(entity->d_name, ".") != 0) && (strcmp(entity->d_name, "..") != 0)) fileCounter++;
    }

    closedir(dir);

    if (fileCounter == 0) return files;

    // Memory allocation for the filepaths
    // NOTE: FilePathList only reports the used entries, so exactly fileCounter
    // entries are allocated and any unused one is released before returning
    files.paths = (char **)RL_CALLOC(fileCounter, sizeof(char *));
    if (files.paths == NULL) return files;

    for (unsigned int i = 0; i < fileCounter; i++) files.paths[i] = (char *)RL_CALLOC(MAX_FILEPATH_LENGTH, sizeof(char));

    // SCAN 2: Read filepaths
    // NOTE: Directory paths are also registered
    ScanDirectoryFiles(dirPath, &files, NULL, fileCounter);

    // Release the entries not filled by the scan (directory could have changed)
    for (unsigned int i = files.count; i < fileCounter; i++) RL_FREE(files.paths[i]);

    if (files.count == 0)
    {
        RL_FREE(files.paths);
        files.paths = NULL;
    }

    return files;
}

// Load directory filepaths with extension filtering and recursive directory scan
// NOTE: Files count is returned by parameter, memory should be freed with UnloadDirectoryFiles()
FilePathList LoadDirectoryFilesEx(const char *basePath, const char *filter, bool scanSubdirs)
{
    FilePathList files = { 0 };

    if (basePath == NULL) return files;

    // NOTE: Scan capacity is fixed, the array is shrunk to the used entries below
    files.paths = (char **)RL_CALLOC(MAX_FILEPATH_CAPACITY, sizeof(char *));
    if (files.paths == NULL) return files;

    for (unsigned int i = 0; i < MAX_FILEPATH_CAPACITY; i++) files.paths[i] = (char *)RL_CALLOC(MAX_FILEPATH_LENGTH, sizeof(char));

    // WARNING: basePath is not checked and it could not exist
    if (scanSubdirs) ScanDirectoryFilesRecursively(basePath, &files, filter, MAX_FILEPATH_CAPACITY);
    else ScanDirectoryFiles(basePath, &files, filter, MAX_FILEPATH_CAPACITY);

    // Release the unused entries and shrink the array to the scanned count
    for (unsigned int i = files.count; i < MAX_FILEPATH_CAPACITY; i++) RL_FREE(files.paths[i]);

    if (files.count == 0)
    {
        RL_FREE(files.paths);
        files.paths = NULL;
        return files;
    }

    char **shrunk = (char **)RL_REALLOC(files.paths, files.count*sizeof(char *));
    if (shrunk != NULL) files.paths = shrunk;

    return files;
}

// Unload directory filepaths
void UnloadDirectoryFiles(FilePathList files)
{
    if (files.paths == NULL) return;

    for (unsigned int i = 0; i < files.count; i++) RL_FREE(files.paths[i]);

    RL_FREE(files.paths);
}

// Get the file count in a directory
unsigned int GetDirectoryFileCount(const char *dirPath)
{
    unsigned int counter = 0;

    if (dirPath == NULL) return 0;

    DIR *dir = opendir(dirPath);
    if (dir == NULL) return 0;

    struct dirent *entity = NULL;
    while ((entity = readdir(dir)) != NULL)
    {
        if ((strcmp(entity->d_name, ".") != 0) && (strcmp(entity->d_name, "..") != 0)) counter++;
    }

    closedir(dir);

    return counter;
}

// Get the file count in a directory with extension filtering and recursive directory scan
unsigned int GetDirectoryFileCountEx(const char *basePath, const char *filter, bool scanSubdirs)
{
    FilePathList files = LoadDirectoryFilesEx(basePath, filter, scanSubdirs);
    unsigned int counter = files.count;
    UnloadDirectoryFiles(files);

    return counter;
}

// Check if a file has been dropped into the window
bool IsFileDropped(void)
{
    return (CORE.window.dropFileCount > 0);
}

// Load dropped filepaths
FilePathList LoadDroppedFiles(void)
{
    FilePathList files = { 0 };

    files.count = CORE.window.dropFileCount;
    files.paths = CORE.window.dropFilepaths;

    return files;
}

// Unload dropped filepaths
void UnloadDroppedFiles(FilePathList files)
{
    // WARNING: files pointers are the same as internal ones
    if (files.count == 0) return;

    for (unsigned int i = 0; i < files.count; i++) RL_FREE(files.paths[i]);

    RL_FREE(files.paths);

    CORE.window.dropFileCount = 0;
    CORE.window.dropFilepaths = NULL;
}

// Get file modification time (last write time)
long GetFileModTime(const char *fileName)
{
    if (fileName == NULL) return 0;

    struct stat result = { 0 };

    if (stat(fileName, &result) == 0) return (long)result.st_mtime;

    return 0;
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Automation events recording and playing
//----------------------------------------------------------------------------------

#define AUTOMATION_EVENTS_VERSION   "1.0"       // Automation events file version

#if SUPPORT_AUTOMATION_EVENTS
// Register one event into the current event list, ignored when the list is full
static void AddAutomationEvent(unsigned int type, int param0, int param1, int param2)
{
    if (currentEventList == NULL) return;
    if (currentEventList->count >= currentEventList->capacity) return;

    AutomationEvent *event = &currentEventList->events[currentEventList->count];

    event->frame = CORE.time.frameCounter;
    event->type = type;
    event->params[0] = param0;
    event->params[1] = param1;
    event->params[2] = param2;
    event->params[3] = 0;

    currentEventList->count++;
}
#endif

// Load automation events list from file, NULL for empty list, capacity = MAX_AUTOMATION_EVENTS
AutomationEventList LoadAutomationEventList(const char *fileName)
{
    AutomationEventList list = { 0 };

    // Allocate the events array, capacity is fixed
    list.events = (AutomationEvent *)RL_CALLOC(MAX_AUTOMATION_EVENTS, sizeof(AutomationEvent));
    if (list.events == NULL) return list;

    list.capacity = MAX_AUTOMATION_EVENTS;

    if (fileName == NULL) return list;      // Empty list requested

    char *text = LoadFileText(fileName);
    if (text == NULL)
    {
        TRACELOG(LOG_WARNING, "AUTOMATION: [%s] Failed to load automation events file", fileName);
        return list;
    }

    char *cursor = text;
    while ((cursor != NULL) && (*cursor != '\0') && (list.count < list.capacity))
    {
        // Skip comment and empty lines
        if ((*cursor != '#') && (*cursor != '\n') && (*cursor != '\r'))
        {
            unsigned int frame = 0;
            unsigned int type = 0;
            int params[4] = { 0 };

            int scanned = sscanf(cursor, "%u %u %i %i %i %i", &frame, &type, &params[0], &params[1], &params[2], &params[3]);

            if (scanned >= 2)
            {
                list.events[list.count].frame = frame;
                list.events[list.count].type = type;
                for (int i = 0; i < 4; i++) list.events[list.count].params[i] = params[i];
                list.count++;
            }
        }

        cursor = strchr(cursor, '\n');
        if (cursor != NULL) cursor++;
    }

    UnloadFileText(text);

    TRACELOG(LOG_INFO, "AUTOMATION: [%s] Events loaded: %i", fileName, list.count);

    return list;
}

// Unload automation events list
void UnloadAutomationEventList(AutomationEventList list)
{
    RL_FREE(list.events);
}

// Export automation events list as text file
bool ExportAutomationEventList(AutomationEventList list, const char *fileName)
{
    if ((fileName == NULL) || (list.events == NULL)) return false;

    // Every event line requires at most 64 bytes, plus a fixed size header
    int capacity = 1024 + 64*(int)list.count;
    char *txtData = (char *)RL_CALLOC(capacity, sizeof(char));
    if (txtData == NULL) return false;

    int byteCount = 0;
    byteCount += snprintf(txtData + byteCount, capacity - byteCount, "#\n");
    byteCount += snprintf(txtData + byteCount, capacity - byteCount, "# Automation events list v%s\n", AUTOMATION_EVENTS_VERSION);
    byteCount += snprintf(txtData + byteCount, capacity - byteCount, "#\n");
    byteCount += snprintf(txtData + byteCount, capacity - byteCount, "# Events count: %i\n", list.count);
    byteCount += snprintf(txtData + byteCount, capacity - byteCount, "# Format: frame type param0 param1 param2 param3\n");
    byteCount += snprintf(txtData + byteCount, capacity - byteCount, "#\n");

    for (unsigned int i = 0; i < list.count; i++)
    {
        byteCount += snprintf(txtData + byteCount, capacity - byteCount, "%u %u %i %i %i %i\n",
                              list.events[i].frame, list.events[i].type,
                              list.events[i].params[0], list.events[i].params[1],
                              list.events[i].params[2], list.events[i].params[3]);
    }

    bool success = SaveFileText(fileName, txtData);

    RL_FREE(txtData);

    return success;
}

// Set automation event list to record to
void SetAutomationEventList(AutomationEventList *list)
{
#if SUPPORT_AUTOMATION_EVENTS
    currentEventList = list;
#else
    (void)list;
    TRACELOG(LOG_WARNING, "AUTOMATION: Automation events support not enabled (SUPPORT_AUTOMATION_EVENTS)");
#endif
}

// Set automation event internal base frame to start recording
void SetAutomationEventBaseFrame(int frame)
{
#if SUPPORT_AUTOMATION_EVENTS
    if (frame < 0) frame = 0;
    automationEventBaseFrame = (unsigned int)frame;
    CORE.time.frameCounter = (unsigned int)frame;
#else
    (void)frame;
#endif
}

// Start recording automation events (AutomationEventList must be set)
void StartAutomationEventRecording(void)
{
#if SUPPORT_AUTOMATION_EVENTS
    if (currentEventList == NULL)
    {
        TRACELOG(LOG_WARNING, "AUTOMATION: No automation event list has been set, use SetAutomationEventList()");
        return;
    }

    automationEventRecording = true;
#else
    TRACELOG(LOG_WARNING, "AUTOMATION: Automation events support not enabled (SUPPORT_AUTOMATION_EVENTS)");
#endif
}

// Stop recording automation events
void StopAutomationEventRecording(void)
{
#if SUPPORT_AUTOMATION_EVENTS
    automationEventRecording = false;
#endif
}

// Play a recorded automation event
void PlayAutomationEvent(AutomationEvent event)
{
#if SUPPORT_AUTOMATION_EVENTS
    // WARNING: When should event be played? After/before/replace PollInputEvents()? -> Up to the user!
    if (automationEventRecording) return;   // Do not mix recording and playing

    switch (event.type)
    {
        // Input event
        case INPUT_KEY_UP:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_KEYBOARD_KEYS)) break;
            CORE.input.keyboard.currentKeyState[event.params[0]] = 0;
        } break;
        case INPUT_KEY_DOWN:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_KEYBOARD_KEYS)) break;

            CORE.input.keyboard.currentKeyState[event.params[0]] = 1;

            if (CORE.input.keyboard.previousKeyState[event.params[0]] == 0)
            {
                if (CORE.input.keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
                {
                    CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount] = event.params[0];
                    CORE.input.keyboard.keyPressedQueueCount++;
                }
            }
        } break;
        case INPUT_MOUSE_BUTTON_UP:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_MOUSE_BUTTONS)) break;
            CORE.input.mouse.currentButtonState[event.params[0]] = 0;
        } break;
        case INPUT_MOUSE_BUTTON_DOWN:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_MOUSE_BUTTONS)) break;
            CORE.input.mouse.currentButtonState[event.params[0]] = 1;
        } break;
        case INPUT_MOUSE_POSITION:
        {
            CORE.input.mouse.currentPosition.x = (float)event.params[0];
            CORE.input.mouse.currentPosition.y = (float)event.params[1];
        } break;
        case INPUT_MOUSE_WHEEL_MOTION:
        {
            CORE.input.mouse.currentWheelMove.x = (float)event.params[0];
            CORE.input.mouse.currentWheelMove.y = (float)event.params[1];
        } break;
        case INPUT_TOUCH_UP:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_TOUCH_POINTS)) break;
            CORE.input.touch.currentTouchState[event.params[0]] = 0;
        } break;
        case INPUT_TOUCH_DOWN:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_TOUCH_POINTS)) break;
            CORE.input.touch.currentTouchState[event.params[0]] = 1;
        } break;
        case INPUT_TOUCH_POSITION:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_TOUCH_POINTS)) break;
            CORE.input.touch.position[event.params[0]].x = (float)event.params[1];
            CORE.input.touch.position[event.params[0]].y = (float)event.params[2];
        } break;
        case INPUT_GAMEPAD_CONNECT:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_GAMEPADS)) break;
            CORE.input.gamepad.ready[event.params[0]] = true;
        } break;
        case INPUT_GAMEPAD_DISCONNECT:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_GAMEPADS)) break;
            CORE.input.gamepad.ready[event.params[0]] = false;
        } break;
        case INPUT_GAMEPAD_BUTTON_UP:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_GAMEPADS)) break;
            if ((event.params[1] < 0) || (event.params[1] >= MAX_GAMEPAD_BUTTONS)) break;
            CORE.input.gamepad.currentButtonState[event.params[0]][event.params[1]] = 0;
        } break;
        case INPUT_GAMEPAD_BUTTON_DOWN:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_GAMEPADS)) break;
            if ((event.params[1] < 0) || (event.params[1] >= MAX_GAMEPAD_BUTTONS)) break;
            CORE.input.gamepad.currentButtonState[event.params[0]][event.params[1]] = 1;
            CORE.input.gamepad.lastButtonPressed = event.params[1];
        } break;
        case INPUT_GAMEPAD_AXIS_MOTION:
        {
            if ((event.params[0] < 0) || (event.params[0] >= MAX_GAMEPADS)) break;
            if ((event.params[1] < 0) || (event.params[1] >= MAX_GAMEPAD_AXES)) break;
            CORE.input.gamepad.axisState[event.params[0]][event.params[1]] = ((float)event.params[2]/32768.0f);
        } break;
#if SUPPORT_GESTURES_SYSTEM
        case INPUT_GESTURE:
        {
            GESTURES.current = (unsigned int)event.params[0];
        } break;
#endif
        // Window event
        case WINDOW_CLOSE: CORE.window.shouldClose = true; break;
        case WINDOW_MAXIMIZE: MaximizeWindow(); break;
        case WINDOW_MINIMIZE: MinimizeWindow(); break;
        case WINDOW_RESIZE: SetWindowSize(event.params[0], event.params[1]); break;
        // Custom event
#if SUPPORT_SCREEN_CAPTURE
        case ACTION_TAKE_SCREENSHOT:
        {
            TakeScreenshot(TextFormat("screenshot%03i.png", screenshotCounter));
            screenshotCounter++;
        } break;
#endif
        case ACTION_SETTARGETFPS: SetTargetFPS(event.params[0]); break;
        default: break;
    }
#else
    (void)event;
    TRACELOG(LOG_WARNING, "AUTOMATION: Automation events support not enabled (SUPPORT_AUTOMATION_EVENTS)");
#endif
}

#if SUPPORT_AUTOMATION_EVENTS
// Record the input state changes of the current frame into the current event list
static void RecordAutomationEvent(void)
{
    if (currentEventList == NULL) return;
    if (CORE.time.frameCounter < automationEventBaseFrame) return;

    // Keyboard input events recording
    for (int key = 0; key < MAX_KEYBOARD_KEYS; key++)
    {
        if (CORE.input.keyboard.previousKeyState[key] && !CORE.input.keyboard.currentKeyState[key]) AddAutomationEvent(INPUT_KEY_UP, key, 0, 0);
        else if (!CORE.input.keyboard.previousKeyState[key] && CORE.input.keyboard.currentKeyState[key]) AddAutomationEvent(INPUT_KEY_DOWN, key, 0, 0);
    }

    // Mouse input currentButtonState record
    for (int button = 0; button < MAX_MOUSE_BUTTONS; button++)
    {
        if (CORE.input.mouse.previousButtonState[button] && !CORE.input.mouse.currentButtonState[button]) AddAutomationEvent(INPUT_MOUSE_BUTTON_UP, button, 0, 0);
        else if (!CORE.input.mouse.previousButtonState[button] && CORE.input.mouse.currentButtonState[button]) AddAutomationEvent(INPUT_MOUSE_BUTTON_DOWN, button, 0, 0);
    }

    // Mouse position and wheel
    if ((CORE.input.mouse.currentPosition.x != CORE.input.mouse.previousPosition.x) ||
        (CORE.input.mouse.currentPosition.y != CORE.input.mouse.previousPosition.y))
    {
        AddAutomationEvent(INPUT_MOUSE_POSITION, (int)CORE.input.mouse.currentPosition.x, (int)CORE.input.mouse.currentPosition.y, 0);
    }

    if ((CORE.input.mouse.currentWheelMove.x != 0.0f) || (CORE.input.mouse.currentWheelMove.y != 0.0f))
    {
        AddAutomationEvent(INPUT_MOUSE_WHEEL_MOTION, (int)CORE.input.mouse.currentWheelMove.x, (int)CORE.input.mouse.currentWheelMove.y, 0);
    }

    // Touch input events recording
    for (int id = 0; id < MAX_TOUCH_POINTS; id++)
    {
        if (CORE.input.touch.previousTouchState[id] && !CORE.input.touch.currentTouchState[id]) AddAutomationEvent(INPUT_TOUCH_UP, id, 0, 0);
        else if (!CORE.input.touch.previousTouchState[id] && CORE.input.touch.currentTouchState[id]) AddAutomationEvent(INPUT_TOUCH_DOWN, id, 0, 0);
    }

    for (int id = 0; id < CORE.input.touch.pointCount; id++)
    {
        AddAutomationEvent(INPUT_TOUCH_POSITION, id, (int)CORE.input.touch.position[id].x, (int)CORE.input.touch.position[id].y);
    }

    // Gamepad input events recording
    for (int gamepad = 0; gamepad < MAX_GAMEPADS; gamepad++)
    {
        if (!CORE.input.gamepad.ready[gamepad]) continue;

        for (int button = 0; button < MAX_GAMEPAD_BUTTONS; button++)
        {
            if (CORE.input.gamepad.previousButtonState[gamepad][button] && !CORE.input.gamepad.currentButtonState[gamepad][button]) AddAutomationEvent(INPUT_GAMEPAD_BUTTON_UP, gamepad, button, 0);
            else if (!CORE.input.gamepad.previousButtonState[gamepad][button] && CORE.input.gamepad.currentButtonState[gamepad][button]) AddAutomationEvent(INPUT_GAMEPAD_BUTTON_DOWN, gamepad, button, 0);
        }

        for (int axis = 0; axis < CORE.input.gamepad.axisCount[gamepad]; axis++)
        {
            float value = CORE.input.gamepad.axisState[gamepad][axis];
            if ((value < -0.1f) || (value > 0.1f)) AddAutomationEvent(INPUT_GAMEPAD_AXIS_MOTION, gamepad, axis, (int)(value*32768.0f));
        }
    }

#if SUPPORT_GESTURES_SYSTEM
    // Gestures input events recording
    if (GESTURES.current != GESTURE_NONE) AddAutomationEvent(INPUT_GESTURE, (int)GESTURES.current, 0, 0);
#endif

    // Window events recording
    if (CORE.window.resizedLastFrame) AddAutomationEvent(WINDOW_RESIZE, CORE.window.screen.width, CORE.window.screen.height, 0);
}
#endif  // SUPPORT_AUTOMATION_EVENTS

//----------------------------------------------------------------------------------
// Module Internal Functions Definition
//----------------------------------------------------------------------------------

// Initialize the time base used by GetTime()
static void InitTimer(void)
{
    CORE.time.base = GetTimePlatform();
    CORE.time.previous = GetTime();     // Get time as double
}

// Set viewport for a provided width and height
static void SetupViewport(int width, int height)
{
    CORE.window.render.width = width;
    CORE.window.render.height = height;

    // Set viewport width and height
    rlViewport(0, 0, width, height);
    rlSetFramebufferWidth(width);
    rlSetFramebufferHeight(height);

    rlMatrixMode(RL_PROJECTION);        // Switch to projection matrix
    rlLoadIdentity();                   // Reset current matrix (projection)

    // Set orthographic projection to current framebuffer size
    // NOTE: Configured top-left corner as (0, 0)
    rlOrtho(0, width, height, 0, 0.0f, 1.0f);

    rlMatrixMode(RL_MODELVIEW);         // Switch back to modelview matrix
    rlLoadIdentity();                   // Reset current matrix (modelview)
}

// Fill shader locs array with the conventional raylib locations
// NOTE: shader->id must be a valid (already loaded) shader program
static void SetupShaderDefaultLocations(Shader *shader)
{
    shader->locs = (int *)RL_CALLOC(RL_MAX_SHADER_LOCATIONS, sizeof(int));
    if (shader->locs == NULL) return;

    // All locations are marked as not-found by default
    for (int i = 0; i < RL_MAX_SHADER_LOCATIONS; i++) shader->locs[i] = -1;

    // Vertex attributes
    shader->locs[SHADER_LOC_VERTEX_POSITION] = rlGetLocationAttrib(shader->id, RL_DEFAULT_SHADER_ATTRIB_NAME_POSITION);
    shader->locs[SHADER_LOC_VERTEX_TEXCOORD01] = rlGetLocationAttrib(shader->id, RL_DEFAULT_SHADER_ATTRIB_NAME_TEXCOORD);
    shader->locs[SHADER_LOC_VERTEX_TEXCOORD02] = rlGetLocationAttrib(shader->id, RL_DEFAULT_SHADER_ATTRIB_NAME_TEXCOORD2);
    shader->locs[SHADER_LOC_VERTEX_NORMAL] = rlGetLocationAttrib(shader->id, RL_DEFAULT_SHADER_ATTRIB_NAME_NORMAL);
    shader->locs[SHADER_LOC_VERTEX_TANGENT] = rlGetLocationAttrib(shader->id, RL_DEFAULT_SHADER_ATTRIB_NAME_TANGENT);
    shader->locs[SHADER_LOC_VERTEX_COLOR] = rlGetLocationAttrib(shader->id, RL_DEFAULT_SHADER_ATTRIB_NAME_COLOR);
    shader->locs[SHADER_LOC_VERTEX_BONEIDS] = rlGetLocationAttrib(shader->id, RL_DEFAULT_SHADER_ATTRIB_NAME_BONEIDS);
    shader->locs[SHADER_LOC_VERTEX_BONEWEIGHTS] = rlGetLocationAttrib(shader->id, RL_DEFAULT_SHADER_ATTRIB_NAME_BONEWEIGHTS);

    // Matrix uniforms
    shader->locs[SHADER_LOC_MATRIX_MVP] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_UNIFORM_NAME_MVP);
    shader->locs[SHADER_LOC_MATRIX_VIEW] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_UNIFORM_NAME_VIEW);
    shader->locs[SHADER_LOC_MATRIX_PROJECTION] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_UNIFORM_NAME_PROJECTION);
    shader->locs[SHADER_LOC_MATRIX_MODEL] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_UNIFORM_NAME_MODEL);
    shader->locs[SHADER_LOC_MATRIX_NORMAL] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_UNIFORM_NAME_NORMAL);
    shader->locs[SHADER_LOC_MATRIX_BONETRANSFORMS] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_UNIFORM_NAME_BONEMATRICES);

    // Color uniform
    shader->locs[SHADER_LOC_COLOR_DIFFUSE] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_UNIFORM_NAME_COLOR);

    // Sampler uniforms
    shader->locs[SHADER_LOC_MAP_DIFFUSE] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_SAMPLER2D_NAME_TEXTURE0);
    shader->locs[SHADER_LOC_MAP_SPECULAR] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_SAMPLER2D_NAME_TEXTURE1);
    shader->locs[SHADER_LOC_MAP_NORMAL] = rlGetLocationUniform(shader->id, RL_DEFAULT_SHADER_SAMPLER2D_NAME_TEXTURE2);
}

// Scan all files and directories in a base path
// WARNING: files.paths[] must be pre-allocated to MAX_FILEPATH_LENGTH bytes per entry
static void ScanDirectoryFiles(const char *basePath, FilePathList *files, const char *filter, unsigned int capacity)
{
    char path[MAX_FILEPATH_LENGTH] = { 0 };

    if ((basePath == NULL) || (files == NULL) || (files->paths == NULL)) return;

    DIR *dir = opendir(basePath);
    if (dir == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: Directory cannot be opened (%s)", basePath);
        return;
    }

    struct dirent *dp = NULL;
    while ((dp = readdir(dir)) != NULL)
    {
        if ((strcmp(dp->d_name, ".") == 0) || (strcmp(dp->d_name, "..") == 0)) continue;

        if (files->count >= capacity)
        {
            TRACELOG(LOG_WARNING, "FILEIO: Maximum filepath scan capacity reached (%u files)", capacity);
            break;
        }

        snprintf(path, MAX_FILEPATH_LENGTH, "%s/%s", basePath, dp->d_name);

        bool keep = false;

        if (filter == NULL) keep = true;
        else if (IsPathFile(path)) keep = IsFileExtension(path, filter);
        else keep = (strstr(filter, DIRECTORY_FILTER_TAG) != NULL);

        if (!keep) continue;

        strncpy(files->paths[files->count], path, MAX_FILEPATH_LENGTH - 1);
        files->paths[files->count][MAX_FILEPATH_LENGTH - 1] = '\0';
        files->count++;
    }

    closedir(dir);
}

// Scan all files and directories recursively from a base path
static void ScanDirectoryFilesRecursively(const char *basePath, FilePathList *files, const char *filter, unsigned int capacity)
{
    char path[MAX_FILEPATH_LENGTH] = { 0 };

    if ((basePath == NULL) || (files == NULL) || (files->paths == NULL)) return;

    DIR *dir = opendir(basePath);
    if (dir == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: Directory cannot be opened (%s)", basePath);
        return;
    }

    struct dirent *dp = NULL;
    while ((dp = readdir(dir)) != NULL)
    {
        if ((strcmp(dp->d_name, ".") == 0) || (strcmp(dp->d_name, "..") == 0)) continue;

        if (files->count >= capacity)
        {
            TRACELOG(LOG_WARNING, "FILEIO: Maximum filepath scan capacity reached (%u files)", capacity);
            break;
        }

        // Construct new path from our base path
        snprintf(path, MAX_FILEPATH_LENGTH, "%s/%s", basePath, dp->d_name);

        if (IsPathFile(path))
        {
            if ((filter == NULL) || IsFileExtension(path, filter))
            {
                strncpy(files->paths[files->count], path, MAX_FILEPATH_LENGTH - 1);
                files->paths[files->count][MAX_FILEPATH_LENGTH - 1] = '\0';
                files->count++;
            }
        }
        else
        {
            if ((filter != NULL) && (strstr(filter, DIRECTORY_FILTER_TAG) != NULL))
            {
                strncpy(files->paths[files->count], path, MAX_FILEPATH_LENGTH - 1);
                files->paths[files->count][MAX_FILEPATH_LENGTH - 1] = '\0';
                files->count++;
            }

            ScanDirectoryFilesRecursively(path, files, filter, capacity);
        }
    }

    closedir(dir);
}
