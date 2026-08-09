/**********************************************************************************************
*
*   rcore_internal - Shared core state and platform backend contract (internal only)
*
*   Every platform backend (rcore_x11.c, rcore_wayland.c, rcore_win32.c,
*   rcore_android.c, rcore_macos.c) implements the Platform* functions declared
*   here and mutates CORE input/window state from its event pump.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#ifndef RCORE_INTERNAL_H
#define RCORE_INTERNAL_H

#include "raylib.h"
#include "rlvk.h"

#include <stdbool.h>
#include <stdint.h>

#define MAX_KEYBOARD_KEYS        512
#define MAX_KEY_PRESSED_QUEUE     16
#define MAX_CHAR_PRESSED_QUEUE    16
#define MAX_MOUSE_BUTTONS          8
#define MAX_GAMEPADS               4
#define MAX_GAMEPAD_AXES           8
#define MAX_GAMEPAD_BUTTONS       32
#define MAX_TOUCH_POINTS           8
#define MAX_FILEPATH_LENGTH     4096
#define MAX_DROPPED_FILES         64

// Trace log macro used across all modules
void TraceLogInternal(int logLevel, const char *text, ...);
#define TRACELOG(level, ...) TraceLog(level, __VA_ARGS__)

typedef struct CoreWindow {
    const char *title;
    unsigned int flags;             // FLAG_* configuration flags (raylib ConfigFlags)
    bool ready;
    bool shouldClose;
    bool fullscreen;
    bool resizedLastFrame;
    bool eventWaiting;              // Wait for events instead of polling
    bool usingFbo;                  // BeginTextureMode active
    struct { int width, height; } display;        // Monitor size
    struct { int width, height; } screen;         // Logical window size
    struct { int width, height; } screenMin;
    struct { int width, height; } screenMax;
    struct { int width, height; } render;         // Framebuffer size (HiDPI aware)
    struct { int x, y; } position;
    struct { int x, y; } previousPosition;        // For fullscreen restore
    struct { int width, height; } previousScreen;
    struct { float x, y; } scale;                 // DPI scale content/framebuffer
    char **dropFilepaths;
    unsigned int dropFileCount;
} CoreWindow;

typedef struct CoreInput {
    struct {
        int exitKey;
        char currentKeyState[MAX_KEYBOARD_KEYS];
        char previousKeyState[MAX_KEYBOARD_KEYS];
        char keyRepeatInFrame[MAX_KEYBOARD_KEYS];
        int keyPressedQueue[MAX_KEY_PRESSED_QUEUE];
        int keyPressedQueueCount;
        int charPressedQueue[MAX_CHAR_PRESSED_QUEUE];
        int charPressedQueueCount;
    } keyboard;
    struct {
        Vector2 offset;
        Vector2 scale;
        Vector2 currentPosition;
        Vector2 previousPosition;
        int cursor;                                // MouseCursor shape
        bool cursorHidden;
        bool cursorOnScreen;
        bool cursorLocked;                         // DisableCursor() relative mode
        char currentButtonState[MAX_MOUSE_BUTTONS];
        char previousButtonState[MAX_MOUSE_BUTTONS];
        Vector2 currentWheelMove;
        Vector2 previousWheelMove;
    } mouse;
    struct {
        int pointCount;
        int pointId[MAX_TOUCH_POINTS];
        Vector2 position[MAX_TOUCH_POINTS];
        char currentTouchState[MAX_TOUCH_POINTS];
        char previousTouchState[MAX_TOUCH_POINTS];
    } touch;
    struct {
        int lastButtonPressed;
        bool ready[MAX_GAMEPADS];
        char name[MAX_GAMEPADS][64];
        int axisCount[MAX_GAMEPADS];
        float axisState[MAX_GAMEPADS][MAX_GAMEPAD_AXES];
        char currentButtonState[MAX_GAMEPADS][MAX_GAMEPAD_BUTTONS];
        char previousButtonState[MAX_GAMEPADS][MAX_GAMEPAD_BUTTONS];
    } gamepad;
} CoreInput;

typedef struct CoreTime {
    double current;
    double previous;
    double update;                  // Time spent in update (frame minus draw/wait)
    double draw;
    double frame;                   // Full frame time
    double target;                  // Target frame time (1/fps), 0 = uncapped
    double base;                    // Time base for GetTime()
    unsigned int frameCounter;
} CoreTime;

typedef struct CoreData {
    CoreWindow window;
    CoreInput input;
    CoreTime time;
} CoreData;

extern CoreData CORE;               // Defined in rcore.c

//----------------------------------------------------------------------------------
// Platform backend contract — implemented by exactly one rcore_<platform>.c
//----------------------------------------------------------------------------------

// Create native window sized CORE.window.screen honoring CORE.window.flags, fill
// CORE.window.display/render/position/scale, then init rlvk via the platform's
// rlvkPlatformGlue (instance extensions + surface callback). Returns 0 on success.
int InitPlatform(void);
void ClosePlatform(void);                          // Destroy window/display connection (after rlvkClose)

void PollInputEventsPlatform(void);                // Pump native events (blocking if CORE.window.eventWaiting)
double GetTimePlatform(void);                      // Monotonic seconds (platform clock)
void WaitTimePlatform(double seconds);             // High-precision sleep
void SwapScreenBufferPlatform(void);               // Present (calls rlvkEndFrame)

// Window control — each maps 1:1 to the public raylib function of similar name
void SetWindowTitlePlatform(const char *title);
void SetWindowPositionPlatform(int x, int y);
void SetWindowSizePlatform(int width, int height);
void SetWindowMinSizePlatform(int width, int height);
void SetWindowMaxSizePlatform(int width, int height);
void SetWindowOpacityPlatform(float opacity);
void SetWindowIconPlatform(Image *images, int count);
void SetWindowFocusedPlatform(void);
void SetWindowMonitorPlatform(int monitor);
void SetWindowStatePlatform(unsigned int flags);   // Apply newly-set flags
void ClearWindowStatePlatform(unsigned int flags); // Apply newly-cleared flags
void ToggleFullscreenPlatform(void);
void ToggleBorderlessWindowedPlatform(void);
void MaximizeWindowPlatform(void);
void MinimizeWindowPlatform(void);
void RestoreWindowPlatform(void);
void *GetWindowHandlePlatform(void);

// Monitors
int GetMonitorCountPlatform(void);
int GetCurrentMonitorPlatform(void);
Vector2 GetMonitorPositionPlatform(int monitor);
int GetMonitorWidthPlatform(int monitor);
int GetMonitorHeightPlatform(int monitor);
int GetMonitorPhysicalWidthPlatform(int monitor);
int GetMonitorPhysicalHeightPlatform(int monitor);
int GetMonitorRefreshRatePlatform(int monitor);
const char *GetMonitorNamePlatform(int monitor);
Vector2 GetWindowScaleDPIPlatform(void);

// Cursor & clipboard
void ShowCursorPlatform(void);
void HideCursorPlatform(void);
void EnableCursorPlatform(void);                   // Unlock (absolute mode)
void DisableCursorPlatform(void);                  // Lock (relative mode)
void SetMouseCursorPlatform(int cursor);           // MouseCursor shape
void SetMousePositionPlatform(int x, int y);
void SetClipboardTextPlatform(const char *text);
const char *GetClipboardTextPlatform(void);
Image GetClipboardImagePlatform(void);

// Misc
void OpenURLPlatform(const char *url);
void SetGamepadVibrationPlatform(int gamepad, float leftMotor, float rightMotor, float duration);
int SetGamepadMappingsPlatform(const char *mappings);

//----------------------------------------------------------------------------------
// Helpers shared across modules (defined in rcore.c / utils)
//----------------------------------------------------------------------------------
unsigned char *DecompressDataInternal(const unsigned char *compData, int compDataSize, int *dataSize);

#endif // RCORE_INTERNAL_H
