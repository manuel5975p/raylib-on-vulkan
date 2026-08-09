/**********************************************************************************************
*
*   rcore_win32 - raylib-on-vulkan platform backend for Windows (raw Win32, no GLFW/SDL)
*
*   Implements the whole platform contract declared in rcore_internal.h on top of the
*   plain Win32 API: RegisterClassExW/CreateWindowExW, a WndProc driven event pump,
*   raw input for relative mouse mode, XInput for gamepads, QPC + winmm for timing and
*   VK_KHR_win32_surface for the Vulkan surface.
*
*   NOTES / CONTRACT REMARKS
*     - <windows.h> and raylib.h collide on a handful of identifiers; the collisions are
*       resolved by external/fix_win32_compatibility.h which MUST be the first include.
*       As a consequence CloseWindow()/ShowCursor()/Rectangle() (the Win32 ones) are not
*       callable from this file; ShowWindow(SW_MINIMIZE) and SetCursor(NULL) are used.
*     - XInput is loaded dynamically (xinput1_4 -> 1_3 -> 9_1_0), so the library needs no
*       xinput import library at link time and still runs on systems without XInput.
*     - vkCreateWin32SurfaceKHR is resolved through vkGetInstanceProcAddr instead of the
*       volk global, so surface creation works no matter whether rlvk called
*       volkLoadInstance() before invoking the glue callback.
*     - PollInputEventsPlatform() owns the full per-frame input bookkeeping (previous
*       state copy, queue reset, gamepad polling) exactly like raylib's platform layers.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#include "fix_win32_compatibility.h"    // MUST come first: <windows.h> + raylib name fixes

#include <shellapi.h>                   // ShellExecuteW, DragAcceptFiles, DragQueryFileW
#include <timeapi.h>                    // timeBeginPeriod/timeEndPeriod (winmm)
#include <hidusage.h>                   // HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE
#include <xinput.h>                     // XINPUT_STATE/XINPUT_VIBRATION types (functions loaded at runtime)

#include "rcore_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//----------------------------------------------------------------------------------
// Allocators (standard raylib fallback pattern)
//----------------------------------------------------------------------------------
#ifndef RL_MALLOC
    #define RL_MALLOC(sz)       malloc(sz)
#endif
#ifndef RL_CALLOC
    #define RL_CALLOC(n, sz)    calloc(n, sz)
#endif
#ifndef RL_REALLOC
    #define RL_REALLOC(p, sz)   realloc(p, sz)
#endif
#ifndef RL_FREE
    #define RL_FREE(p)          free(p)
#endif

// Clipboard image support (DIB -> BMP in memory), needs RL_MALLOC/TRACELOG defined above.
// The vendored header re-declares parts of the Win32 API so it can be used *without*
// <windows.h>; the guards below tell it that windows.h is already present. They are the
// MSVC-style include guards the header tests for (mingw-w64 uses different ones).
#define _WINUSER_
#define WINUSER_ALREADY_INCLUDED
#define _WINBASE_
#define WINBASE_ALREADY_INCLUDED
#define _WINGDI_
#define WINGDI_ALREADY_INCLUDED
#define WIN32_CLIPBOARD_IMPLEMENTATION
#include "win32_clipboard.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "volk.h"

//----------------------------------------------------------------------------------
// Defines not guaranteed to be present in older SDK headers
//----------------------------------------------------------------------------------
#ifndef WM_MOUSEHWHEEL
    #define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef WM_DPICHANGED
    #define WM_DPICHANGED 0x02E0
#endif
#ifndef USER_DEFAULT_SCREEN_DPI
    #define USER_DEFAULT_SCREEN_DPI 96
#endif
#ifndef WHEEL_DELTA
    #define WHEEL_DELTA 120
#endif
#ifndef LWA_ALPHA
    #define LWA_ALPHA 0x00000002
#endif
#ifndef ENUM_CURRENT_SETTINGS
    #define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#endif
#ifndef MAPVK_VSC_TO_VK_EX
    #define MAPVK_VSC_TO_VK_EX 3
#endif
#ifndef KF_EXTENDED
    #define KF_EXTENDED 0x0100
#endif
#ifndef XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    #define XINPUT_GAMEPAD_TRIGGER_THRESHOLD 30
#endif

#define RAYVK_WNDCLASS_NAME     L"RAYVULKAN_WINDOW"
#define RAYVK_MAX_MONITORS      16
#define RAYVK_CLIPBOARD_MAX     (16*1024)

// DPI awareness contexts (declared here to avoid a hard dependency on a recent SDK)
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    #define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif

typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef BOOL (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef DWORD (WINAPI *PFN_XInputGetStateProc)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *PFN_XInputSetStateProc)(DWORD, XINPUT_VIBRATION *);

//----------------------------------------------------------------------------------
// Module state
//----------------------------------------------------------------------------------
typedef struct MonitorInfoEntry {
    HMONITOR handle;
    RECT bounds;                        // Virtual-desktop rectangle
    WCHAR deviceName[CCHDEVICENAME];    // e.g. "\\\\.\\DISPLAY1"
    BOOL primary;
} MonitorInfoEntry;

typedef struct PlatformData {
    HINSTANCE instance;
    HWND handle;

    HCURSOR cursors[11];                // Indexed by raylib MouseCursor
    HCURSOR currentCursor;
    HICON bigIcon;
    HICON smallIcon;

    DWORD style;
    DWORD styleEx;
    WINDOWPLACEMENT restorePlacement;   // Saved before fullscreen/borderless
    bool placementSaved;

    bool classRegistered;
    bool cursorTracked;                 // TrackMouseEvent armed
    bool rawInputActive;
    bool timerPeriodSet;

    LARGE_INTEGER timerFrequency;

    // Monitors (rebuilt on demand, primary first then sorted by position)
    MonitorInfoEntry monitors[RAYVK_MAX_MONITORS];
    int monitorCount;

    // Gamepads (XInput)
    HMODULE xinputLib;
    PFN_XInputGetStateProc XInputGetStateFn;
    PFN_XInputSetStateProc XInputSetStateFn;
    double vibrationEnd[MAX_GAMEPADS];
    unsigned int gamepadRescanCounter;

    // Cached DPI helpers
    PFN_GetDpiForWindow GetDpiForWindowFn;
    PFN_AdjustWindowRectExForDpi AdjustWindowRectExForDpiFn;

    char *clipboardText;                // Owned, returned by GetClipboardTextPlatform()
    char monitorName[128];              // Static return buffer (raylib semantics)
} PlatformData;

static PlatformData platform = { 0 };

//----------------------------------------------------------------------------------
// Module internal functions declaration
//----------------------------------------------------------------------------------
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static uint64_t CreateSurfaceWin32(void *vkInstance, void *userData);

static WCHAR *WideFromUtf8(const char *text);              // RL_FREE the result
static char *Utf8FromWide(const WCHAR *text);              // RL_FREE the result

static int TranslateKey(WPARAM wParam, LPARAM lParam);     // VK_* -> raylib KEY_*, -1 to swallow
static void ResetInputStates(void);                        // Release everything (focus loss)

static void RefreshMonitors(void);
static int MonitorIndexFromHandle(HMONITOR handle);
static bool MonitorIsValid(int monitor);

static void UpdateWindowMetrics(void);                     // screen/render/scale from client rect
static void ApplyWindowStyles(bool resizeToScreen);
static DWORD BuildWindowStyle(unsigned int flags);
static DWORD BuildWindowStyleEx(unsigned int flags);
static void AdjustRectForWindow(RECT *rect);
static void ClipCursorToWindow(bool clip);
static void SetRawMouseInput(bool enabled);
static void CenterCursorInWindow(void);
static void UpdateGamepads(void);
static void LoadXInput(void);
static HICON CreateIconFromImage(Image image, int desiredWidth, int desiredHeight, bool isCursorUnused);
static void RegisterMouseButton(int button, bool down);
static Vector2 PixelsToLogical(int x, int y);

//----------------------------------------------------------------------------------
// Small helpers
//----------------------------------------------------------------------------------

// Convert a UTF-8 string to a freshly allocated UTF-16 string, NULL on failure
static WCHAR *WideFromUtf8(const char *text)
{
    if (text == NULL) return NULL;

    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (count <= 0) return NULL;

    WCHAR *wide = (WCHAR *)RL_CALLOC((size_t)count, sizeof(WCHAR));
    if (wide == NULL) return NULL;

    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count) <= 0)
    {
        RL_FREE(wide);
        return NULL;
    }

    return wide;
}

// Convert a UTF-16 string to a freshly allocated UTF-8 string, NULL on failure
static char *Utf8FromWide(const WCHAR *text)
{
    if (text == NULL) return NULL;

    int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (count <= 0) return NULL;

    char *utf8 = (char *)RL_CALLOC((size_t)count, 1);
    if (utf8 == NULL) return NULL;

    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, count, NULL, NULL) <= 0)
    {
        RL_FREE(utf8);
        return NULL;
    }

    return utf8;
}

// Convert client-area pixels into raylib logical screen coordinates
static Vector2 PixelsToLogical(int x, int y)
{
    Vector2 result = { (float)x, (float)y };

    if ((CORE.window.scale.x > 0.0f) && (CORE.window.scale.y > 0.0f) &&
        ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0))
    {
        result.x /= CORE.window.scale.x;
        result.y /= CORE.window.scale.y;
    }

    return result;
}

// Register a mouse button state change and mirror it into the touch emulation slot
static void RegisterMouseButton(int button, bool down)
{
    if ((button < 0) || (button >= MAX_MOUSE_BUTTONS)) return;

    CORE.input.mouse.currentButtonState[button] = down? 1 : 0;

    if (button < MAX_TOUCH_POINTS)
    {
        CORE.input.touch.currentTouchState[button] = down? 1 : 0;
        CORE.input.touch.position[button] = CORE.input.mouse.currentPosition;
        CORE.input.touch.pointId[button] = button;
    }

    // Touch point count mirrors pressed emulated points
    int count = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) if (CORE.input.touch.currentTouchState[i] == 1) count++;
    CORE.input.touch.pointCount = count;
}

// Release every key/button, used when the window loses focus
static void ResetInputStates(void)
{
    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++)
    {
        CORE.input.keyboard.currentKeyState[i] = 0;
        CORE.input.keyboard.keyRepeatInFrame[i] = 0;
    }

    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) CORE.input.mouse.currentButtonState[i] = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.input.touch.currentTouchState[i] = 0;
    CORE.input.touch.pointCount = 0;
}

//----------------------------------------------------------------------------------
// Keyboard translation
//----------------------------------------------------------------------------------

// Map a Win32 virtual key + message lParam to a raylib KeyboardKey
// Returns KEY_NULL for unmapped keys and -1 for the synthetic AltGr control key
static int TranslateKey(WPARAM wParam, LPARAM lParam)
{
    const bool extended = ((HIWORD(lParam) & KF_EXTENDED) != 0);

    switch (wParam)
    {
        case VK_SHIFT:
        {
            UINT scancode = (UINT)((lParam >> 16) & 0xFF);
            UINT vk = MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
            if (vk == VK_RSHIFT) return KEY_RIGHT_SHIFT;
            return KEY_LEFT_SHIFT;
        }
        case VK_CONTROL:
        {
            if (extended) return KEY_RIGHT_CONTROL;

            // AltGr sends a synthetic left control right before the right alt: swallow it
            MSG next = { 0 };
            LONG time = GetMessageTime();
            if (PeekMessageW(&next, NULL, 0, 0, PM_NOREMOVE))
            {
                bool isKeyMsg = ((next.message == WM_KEYDOWN) || (next.message == WM_SYSKEYDOWN) ||
                                 (next.message == WM_KEYUP) || (next.message == WM_SYSKEYUP));
                if (isKeyMsg && (next.wParam == VK_MENU) && ((HIWORD(next.lParam) & KF_EXTENDED) != 0) &&
                    ((LONG)next.time == time)) return -1;
            }

            return KEY_LEFT_CONTROL;
        }
        case VK_MENU: return extended? KEY_RIGHT_ALT : KEY_LEFT_ALT;
        case VK_RETURN: return extended? KEY_KP_ENTER : KEY_ENTER;
        case VK_LSHIFT: return KEY_LEFT_SHIFT;
        case VK_RSHIFT: return KEY_RIGHT_SHIFT;
        case VK_LCONTROL: return KEY_LEFT_CONTROL;
        case VK_RCONTROL: return KEY_RIGHT_CONTROL;
        case VK_LMENU: return KEY_LEFT_ALT;
        case VK_RMENU: return KEY_RIGHT_ALT;
        case VK_LWIN: return KEY_LEFT_SUPER;
        case VK_RWIN: return KEY_RIGHT_SUPER;
        case VK_APPS: return KEY_KB_MENU;

        case VK_SPACE: return KEY_SPACE;
        case VK_ESCAPE: return KEY_ESCAPE;
        case VK_TAB: return KEY_TAB;
        case VK_BACK: return KEY_BACKSPACE;
        case VK_INSERT: return KEY_INSERT;
        case VK_DELETE: return KEY_DELETE;
        case VK_RIGHT: return KEY_RIGHT;
        case VK_LEFT: return KEY_LEFT;
        case VK_DOWN: return KEY_DOWN;
        case VK_UP: return KEY_UP;
        case VK_PRIOR: return KEY_PAGE_UP;
        case VK_NEXT: return KEY_PAGE_DOWN;
        case VK_HOME: return KEY_HOME;
        case VK_END: return KEY_END;
        case VK_CAPITAL: return KEY_CAPS_LOCK;
        case VK_SCROLL: return KEY_SCROLL_LOCK;
        case VK_NUMLOCK: return KEY_NUM_LOCK;
        case VK_SNAPSHOT: return KEY_PRINT_SCREEN;
        case VK_PAUSE: return KEY_PAUSE;

        case VK_F1: return KEY_F1;
        case VK_F2: return KEY_F2;
        case VK_F3: return KEY_F3;
        case VK_F4: return KEY_F4;
        case VK_F5: return KEY_F5;
        case VK_F6: return KEY_F6;
        case VK_F7: return KEY_F7;
        case VK_F8: return KEY_F8;
        case VK_F9: return KEY_F9;
        case VK_F10: return KEY_F10;
        case VK_F11: return KEY_F11;
        case VK_F12: return KEY_F12;

        case VK_NUMPAD0: return KEY_KP_0;
        case VK_NUMPAD1: return KEY_KP_1;
        case VK_NUMPAD2: return KEY_KP_2;
        case VK_NUMPAD3: return KEY_KP_3;
        case VK_NUMPAD4: return KEY_KP_4;
        case VK_NUMPAD5: return KEY_KP_5;
        case VK_NUMPAD6: return KEY_KP_6;
        case VK_NUMPAD7: return KEY_KP_7;
        case VK_NUMPAD8: return KEY_KP_8;
        case VK_NUMPAD9: return KEY_KP_9;
        case VK_DECIMAL: return KEY_KP_DECIMAL;
        case VK_DIVIDE: return KEY_KP_DIVIDE;
        case VK_MULTIPLY: return KEY_KP_MULTIPLY;
        case VK_SUBTRACT: return KEY_KP_SUBTRACT;
        case VK_ADD: return KEY_KP_ADD;

        case VK_OEM_1: return KEY_SEMICOLON;      // US layout: ;:
        case VK_OEM_PLUS: return KEY_EQUAL;
        case VK_OEM_COMMA: return KEY_COMMA;
        case VK_OEM_MINUS: return KEY_MINUS;
        case VK_OEM_PERIOD: return KEY_PERIOD;
        case VK_OEM_2: return KEY_SLASH;          // US layout: /?
        case VK_OEM_3: return KEY_GRAVE;          // US layout: `~
        case VK_OEM_4: return KEY_LEFT_BRACKET;
        case VK_OEM_5: return KEY_BACKSLASH;
        case VK_OEM_6: return KEY_RIGHT_BRACKET;
        case VK_OEM_7: return KEY_APOSTROPHE;
        default: break;
    }

    // Digits and letters share the ASCII values with raylib key codes
    if ((wParam >= '0') && (wParam <= '9')) return (int)wParam;
    if ((wParam >= 'A') && (wParam <= 'Z')) return (int)wParam;

    return KEY_NULL;
}

//----------------------------------------------------------------------------------
// Monitors
//----------------------------------------------------------------------------------
static BOOL CALLBACK MonitorEnumProc(HMONITOR handle, HDC dc, LPRECT rect, LPARAM data)
{
    (void)dc; (void)rect; (void)data;

    if (platform.monitorCount >= RAYVK_MAX_MONITORS) return FALSE;

    MONITORINFOEXW info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(handle, (LPMONITORINFO)&info)) return TRUE;

    MonitorInfoEntry *entry = &platform.monitors[platform.monitorCount];
    entry->handle = handle;
    entry->bounds = info.rcMonitor;
    entry->primary = ((info.dwFlags & MONITORINFOF_PRIMARY) != 0)? TRUE : FALSE;
    memcpy(entry->deviceName, info.szDevice, sizeof(entry->deviceName));
    platform.monitorCount++;

    return TRUE;
}

// Rebuild the monitor list in a deterministic order: primary first, then by (x, y)
static void RefreshMonitors(void)
{
    platform.monitorCount = 0;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);

    for (int i = 1; i < platform.monitorCount; i++)
    {
        MonitorInfoEntry key = platform.monitors[i];
        int j = i - 1;
        while (j >= 0)
        {
            const MonitorInfoEntry *a = &platform.monitors[j];
            bool aFirst = false;
            if (a->primary != key.primary) aFirst = (a->primary != 0);
            else if (a->bounds.left != key.bounds.left) aFirst = (a->bounds.left < key.bounds.left);
            else aFirst = (a->bounds.top <= key.bounds.top);

            if (aFirst) break;
            platform.monitors[j + 1] = platform.monitors[j];
            j--;
        }
        platform.monitors[j + 1] = key;
    }
}

static int MonitorIndexFromHandle(HMONITOR handle)
{
    for (int i = 0; i < platform.monitorCount; i++)
    {
        if (platform.monitors[i].handle == handle) return i;
    }
    return 0;
}

static bool MonitorIsValid(int monitor)
{
    return ((monitor >= 0) && (monitor < platform.monitorCount));
}

//----------------------------------------------------------------------------------
// Window metrics / styles
//----------------------------------------------------------------------------------

// Recompute render (pixels), screen (logical) and DPI scale from the current client area
static void UpdateWindowMetrics(void)
{
    if (platform.handle == NULL) return;

    RECT client = { 0, 0, 0, 0 };
    GetClientRect(platform.handle, &client);

    int pixelWidth = (int)(client.right - client.left);
    int pixelHeight = (int)(client.bottom - client.top);
    if (pixelWidth < 1) pixelWidth = 1;
    if (pixelHeight < 1) pixelHeight = 1;

    float scale = 1.0f;
    if ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0)
    {
        UINT dpi = USER_DEFAULT_SCREEN_DPI;
        if (platform.GetDpiForWindowFn != NULL) dpi = platform.GetDpiForWindowFn(platform.handle);
        if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
        scale = (float)dpi/(float)USER_DEFAULT_SCREEN_DPI;
    }

    CORE.window.scale.x = scale;
    CORE.window.scale.y = scale;
    CORE.window.render.width = pixelWidth;
    CORE.window.render.height = pixelHeight;
    CORE.window.screen.width = (int)((float)pixelWidth/scale);
    CORE.window.screen.height = (int)((float)pixelHeight/scale);
    if (CORE.window.screen.width < 1) CORE.window.screen.width = 1;
    if (CORE.window.screen.height < 1) CORE.window.screen.height = 1;

    POINT origin = { 0, 0 };
    if (ClientToScreen(platform.handle, &origin))
    {
        CORE.window.position.x = origin.x;
        CORE.window.position.y = origin.y;
    }
}

static DWORD BuildWindowStyle(unsigned int flags)
{
    if (CORE.window.fullscreen || ((flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0)) return WS_POPUP;

    DWORD style = WS_CLIPSIBLINGS|WS_CLIPCHILDREN;

    if ((flags & FLAG_WINDOW_UNDECORATED) > 0) style |= WS_POPUP;
    else
    {
        style |= WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
        if ((flags & FLAG_WINDOW_RESIZABLE) > 0) style |= WS_MAXIMIZEBOX|WS_THICKFRAME;
    }

    return style;
}

static DWORD BuildWindowStyleEx(unsigned int flags)
{
    DWORD styleEx = WS_EX_APPWINDOW;

    if ((flags & FLAG_WINDOW_TOPMOST) > 0) styleEx |= WS_EX_TOPMOST;
    if ((flags & FLAG_WINDOW_TRANSPARENT) > 0) styleEx |= WS_EX_LAYERED;
    if ((flags & FLAG_WINDOW_MOUSE_PASSTHROUGH) > 0) styleEx |= WS_EX_TRANSPARENT|WS_EX_LAYERED;

    return styleEx;
}

// Apply the styles derived from CORE.window.flags to the live window
static void ApplyWindowStyles(bool resizeToScreen)
{
    if (platform.handle == NULL) return;

    platform.style = BuildWindowStyle(CORE.window.flags);
    platform.styleEx = BuildWindowStyleEx(CORE.window.flags);

    DWORD visible = (DWORD)(GetWindowLongPtrW(platform.handle, GWL_STYLE) & WS_VISIBLE);
    SetWindowLongPtrW(platform.handle, GWL_STYLE, (LONG_PTR)(platform.style|visible));
    SetWindowLongPtrW(platform.handle, GWL_EXSTYLE, (LONG_PTR)platform.styleEx);

    UINT posFlags = SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE;

    if (resizeToScreen)
    {
        RECT rect = { 0, 0, 0, 0 };
        rect.right = (LONG)(CORE.window.screen.width*CORE.window.scale.x);
        rect.bottom = (LONG)(CORE.window.screen.height*CORE.window.scale.y);
        AdjustRectForWindow(&rect);

        SetWindowPos(platform.handle, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        posFlags = 0;
    }

    if (posFlags != 0) SetWindowPos(platform.handle, NULL, 0, 0, 0, 0, posFlags);

    HWND zOrder = ((CORE.window.flags & FLAG_WINDOW_TOPMOST) > 0)? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(platform.handle, zOrder, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);

    UpdateWindowMetrics();
}

// Grow a client rectangle into a window rectangle using the current styles (DPI aware)
// rect nonnull
static void AdjustRectForWindow(RECT *rect)
{
    if ((platform.AdjustWindowRectExForDpiFn != NULL) && (platform.GetDpiForWindowFn != NULL) &&
        (platform.handle != NULL) && ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0))
    {
        UINT dpi = platform.GetDpiForWindowFn(platform.handle);
        if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
        if (platform.AdjustWindowRectExForDpiFn(rect, platform.style, FALSE, platform.styleEx, dpi)) return;
    }

    AdjustWindowRectEx(rect, platform.style, FALSE, platform.styleEx);
}

// Confine or release the cursor inside the client area (used by DisableCursor)
static void ClipCursorToWindow(bool clip)
{
    if (!clip || (platform.handle == NULL))
    {
        ClipCursor(NULL);
        return;
    }

    RECT client = { 0, 0, 0, 0 };
    GetClientRect(platform.handle, &client);

    POINT topLeft = { client.left, client.top };
    POINT bottomRight = { client.right, client.bottom };
    ClientToScreen(platform.handle, &topLeft);
    ClientToScreen(platform.handle, &bottomRight);

    RECT screenRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    ClipCursor(&screenRect);
}

// Enable/disable WM_INPUT relative mouse deltas
static void SetRawMouseInput(bool enabled)
{
    if (platform.handle == NULL) return;
    if (enabled == platform.rawInputActive) return;

    RAWINPUTDEVICE device;
    memset(&device, 0, sizeof(device));
    device.usUsagePage = HID_USAGE_PAGE_GENERIC;
    device.usUsage = HID_USAGE_GENERIC_MOUSE;
    device.dwFlags = enabled? 0 : RIDEV_REMOVE;
    device.hwndTarget = enabled? platform.handle : NULL;

    if (RegisterRawInputDevices(&device, 1, sizeof(device))) platform.rawInputActive = enabled;
    else TRACELOG(LOG_WARNING, "PLATFORM: Failed to %s raw mouse input", enabled? "register" : "unregister");
}

static void CenterCursorInWindow(void)
{
    if (platform.handle == NULL) return;

    RECT client = { 0, 0, 0, 0 };
    GetClientRect(platform.handle, &client);

    POINT center = { (client.right - client.left)/2, (client.bottom - client.top)/2 };
    ClientToScreen(platform.handle, &center);
    SetCursorPos(center.x, center.y);
}

//----------------------------------------------------------------------------------
// Window procedure
//----------------------------------------------------------------------------------
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CLOSE:
        {
            CORE.window.shouldClose = true;
            return 0;                       // Do not destroy the window here, rcore owns the lifetime
        }
        case WM_DESTROY:
        {
            CORE.window.shouldClose = true;
            return 0;
        }
        case WM_ERASEBKGND: return 1;       // Vulkan owns the client area, avoid GDI flicker

        case WM_SIZE:
        {
            if (wParam == SIZE_MINIMIZED)
            {
                CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
                return 0;
            }

            CORE.window.flags &= ~FLAG_WINDOW_MINIMIZED;
            if (wParam == SIZE_MAXIMIZED) CORE.window.flags |= FLAG_WINDOW_MAXIMIZED;
            else if (wParam == SIZE_RESTORED) CORE.window.flags &= ~FLAG_WINDOW_MAXIMIZED;

            int previousWidth = CORE.window.render.width;
            int previousHeight = CORE.window.render.height;
            UpdateWindowMetrics();

            if ((CORE.window.render.width != previousWidth) || (CORE.window.render.height != previousHeight))
            {
                CORE.window.resizedLastFrame = true;
                if (rlvkIsReady()) rlvkResize(CORE.window.render.width, CORE.window.render.height);
            }

            if (CORE.input.mouse.cursorLocked) ClipCursorToWindow(true);
            return 0;
        }
        case WM_MOVE:
        {
            CORE.window.position.x = (int)(short)LOWORD(lParam);
            CORE.window.position.y = (int)(short)HIWORD(lParam);
            if (CORE.input.mouse.cursorLocked) ClipCursorToWindow(true);
            return 0;
        }
        case WM_SETFOCUS:
        {
            CORE.window.flags &= ~FLAG_WINDOW_UNFOCUSED;
            if (CORE.input.mouse.cursorLocked)
            {
                ClipCursorToWindow(true);
                SetRawMouseInput(true);
            }
            return 0;
        }
        case WM_KILLFOCUS:
        {
            CORE.window.flags |= FLAG_WINDOW_UNFOCUSED;
            ResetInputStates();
            if (CORE.input.mouse.cursorLocked)
            {
                ClipCursorToWindow(false);
                SetRawMouseInput(false);
            }
            return 0;
        }
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO *info = (MINMAXINFO *)lParam;
            float scale = ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0)? CORE.window.scale.x : 1.0f;
            if (scale <= 0.0f) scale = 1.0f;

            if ((CORE.window.screenMin.width > 0) && (CORE.window.screenMin.height > 0))
            {
                RECT rect = { 0, 0, (LONG)(CORE.window.screenMin.width*scale), (LONG)(CORE.window.screenMin.height*scale) };
                AdjustRectForWindow(&rect);
                info->ptMinTrackSize.x = rect.right - rect.left;
                info->ptMinTrackSize.y = rect.bottom - rect.top;
            }

            if ((CORE.window.screenMax.width > 0) && (CORE.window.screenMax.height > 0))
            {
                RECT rect = { 0, 0, (LONG)(CORE.window.screenMax.width*scale), (LONG)(CORE.window.screenMax.height*scale) };
                AdjustRectForWindow(&rect);
                info->ptMaxTrackSize.x = rect.right - rect.left;
                info->ptMaxTrackSize.y = rect.bottom - rect.top;
            }

            return 0;
        }
        case WM_DPICHANGED:
        {
            if ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0)
            {
                const RECT *suggested = (const RECT *)lParam;
                SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER|SWP_NOACTIVATE);
                UpdateWindowMetrics();
                TRACELOG(LOG_INFO, "PLATFORM: DPI changed, content scale is now %.2f", (double)CORE.window.scale.x);
            }
            return 0;
        }
        case WM_SETCURSOR:
        {
            if (LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(CORE.input.mouse.cursorHidden? NULL : platform.currentCursor);
                return TRUE;
            }
            break;
        }
        case WM_SYSCOMMAND:
        {
            // Block the ALT/F10 menu (steals key input) and the screensaver while fullscreen
            if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
            if (CORE.window.fullscreen &&
                (((wParam & 0xFFF0) == SC_SCREENSAVE) || ((wParam & 0xFFF0) == SC_MONITORPOWER))) return 0;
            break;
        }

        //--------------------------------------------------------------------------
        // Keyboard
        //--------------------------------------------------------------------------
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            int key = TranslateKey(wParam, lParam);
            if (key < 0) return 0;                              // Synthetic AltGr control
            if ((key > 0) && (key < MAX_KEYBOARD_KEYS))
            {
                bool repeat = ((lParam & 0x40000000) != 0);
                if (repeat) CORE.input.keyboard.keyRepeatInFrame[key] = 1;

                CORE.input.keyboard.currentKeyState[key] = 1;

                if (CORE.input.keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
                {
                    CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount] = key;
                    CORE.input.keyboard.keyPressedQueueCount++;
                }
            }

            if (msg == WM_SYSKEYDOWN) break;                    // Let DefWindowProc see system keys
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            int key = TranslateKey(wParam, lParam);
            if (key < 0) return 0;

            if (wParam == VK_SHIFT)
            {
                // Windows does not report the release of the second shift key
                CORE.input.keyboard.currentKeyState[KEY_LEFT_SHIFT] = 0;
                CORE.input.keyboard.currentKeyState[KEY_RIGHT_SHIFT] = 0;
            }
            else if (wParam == VK_SNAPSHOT)
            {
                // Print screen only ever reports the release: synthesize the press
                CORE.input.keyboard.currentKeyState[KEY_PRINT_SCREEN] = 1;
                if (CORE.input.keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
                {
                    CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount] = KEY_PRINT_SCREEN;
                    CORE.input.keyboard.keyPressedQueueCount++;
                }
            }
            else if ((key > 0) && (key < MAX_KEYBOARD_KEYS)) CORE.input.keyboard.currentKeyState[key] = 0;

            if (msg == WM_SYSKEYUP) break;
            return 0;
        }
        case WM_CHAR:
        case WM_SYSCHAR:
        {
            static WCHAR highSurrogate = 0;
            WCHAR unit = (WCHAR)wParam;
            int codepoint = 0;

            if ((unit >= 0xD800) && (unit <= 0xDBFF))
            {
                highSurrogate = unit;
                return 0;
            }

            if ((unit >= 0xDC00) && (unit <= 0xDFFF))
            {
                if (highSurrogate == 0) return 0;
                codepoint = 0x10000 + (((int)(highSurrogate - 0xD800)) << 10) + (int)(unit - 0xDC00);
                highSurrogate = 0;
            }
            else
            {
                highSurrogate = 0;
                codepoint = (int)unit;
            }

            if ((codepoint >= 32) && (codepoint != 127) &&
                (CORE.input.keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE))
            {
                CORE.input.keyboard.charPressedQueue[CORE.input.keyboard.charPressedQueueCount] = codepoint;
                CORE.input.keyboard.charPressedQueueCount++;
            }

            if (msg == WM_SYSCHAR) break;
            return 0;
        }
        case WM_UNICHAR:
        {
            if (wParam == UNICODE_NOCHAR) return TRUE;          // Advertise UTF-32 support

            int codepoint = (int)wParam;
            if ((codepoint >= 32) && (codepoint != 127) &&
                (CORE.input.keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE))
            {
                CORE.input.keyboard.charPressedQueue[CORE.input.keyboard.charPressedQueueCount] = codepoint;
                CORE.input.keyboard.charPressedQueueCount++;
            }
            return 0;
        }

        //--------------------------------------------------------------------------
        // Mouse
        //--------------------------------------------------------------------------
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int button = MOUSE_BUTTON_LEFT;
            if ((msg == WM_RBUTTONDOWN) || (msg == WM_RBUTTONUP)) button = MOUSE_BUTTON_RIGHT;
            else if ((msg == WM_MBUTTONDOWN) || (msg == WM_MBUTTONUP)) button = MOUSE_BUTTON_MIDDLE;
            else if ((msg == WM_XBUTTONDOWN) || (msg == WM_XBUTTONUP))
                button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)? MOUSE_BUTTON_SIDE : MOUSE_BUTTON_EXTRA;

            bool down = ((msg == WM_LBUTTONDOWN) || (msg == WM_RBUTTONDOWN) ||
                         (msg == WM_MBUTTONDOWN) || (msg == WM_XBUTTONDOWN));

            if (down)
            {
                bool anyDown = false;
                for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) if (CORE.input.mouse.currentButtonState[i] == 1) anyDown = true;
                if (!anyDown) SetCapture(hwnd);
            }

            RegisterMouseButton(button, down);

            if (!down)
            {
                bool anyDown = false;
                for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) if (CORE.input.mouse.currentButtonState[i] == 1) anyDown = true;
                if (!anyDown && (GetCapture() == hwnd)) ReleaseCapture();
            }

            if ((msg == WM_XBUTTONDOWN) || (msg == WM_XBUTTONUP)) return TRUE;
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!platform.cursorTracked)
            {
                TRACKMOUSEEVENT track;
                memset(&track, 0, sizeof(track));
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                platform.cursorTracked = true;
                CORE.input.mouse.cursorOnScreen = true;
            }

            if (!CORE.input.mouse.cursorLocked)
            {
                CORE.input.mouse.currentPosition = PixelsToLogical((int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
                if (CORE.input.touch.pointCount > 0) CORE.input.touch.position[0] = CORE.input.mouse.currentPosition;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
        {
            platform.cursorTracked = false;
            CORE.input.mouse.cursorOnScreen = false;
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            CORE.input.mouse.currentWheelMove.y += (float)((short)HIWORD(wParam))/(float)WHEEL_DELTA;
            return 0;
        }
        case WM_MOUSEHWHEEL:
        {
            CORE.input.mouse.currentWheelMove.x -= (float)((short)HIWORD(wParam))/(float)WHEEL_DELTA;
            return 0;
        }
        case WM_INPUT:
        {
            if (!CORE.input.mouse.cursorLocked) break;

            UINT size = 0;
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER)) != 0) break;
            if (size == 0) break;

            static BYTE staticBuffer[1024];
            BYTE *buffer = staticBuffer;
            BYTE *heapBuffer = NULL;
            if (size > sizeof(staticBuffer))
            {
                heapBuffer = (BYTE *)RL_MALLOC(size);
                if (heapBuffer == NULL) break;
                buffer = heapBuffer;
            }

            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size)
            {
                RAWINPUT *raw = (RAWINPUT *)buffer;
                if (raw->header.dwType == RIM_TYPEMOUSE)
                {
                    float deltaX = 0.0f;
                    float deltaY = 0.0f;

                    if ((raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
                    {
                        // Absolute devices (remote desktop, tablets) report normalized coordinates
                        static LONG lastAbsX = 0;
                        static LONG lastAbsY = 0;
                        static bool hasLastAbs = false;

                        if (hasLastAbs)
                        {
                            deltaX = (float)(raw->data.mouse.lLastX - lastAbsX);
                            deltaY = (float)(raw->data.mouse.lLastY - lastAbsY);
                        }
                        lastAbsX = raw->data.mouse.lLastX;
                        lastAbsY = raw->data.mouse.lLastY;
                        hasLastAbs = true;
                    }
                    else
                    {
                        deltaX = (float)raw->data.mouse.lLastX;
                        deltaY = (float)raw->data.mouse.lLastY;
                    }

                    CORE.input.mouse.currentPosition.x += deltaX;
                    CORE.input.mouse.currentPosition.y += deltaY;
                }
            }

            if (heapBuffer != NULL) RL_FREE(heapBuffer);
            break;                                  // WM_INPUT must reach DefWindowProc for cleanup
        }

        //--------------------------------------------------------------------------
        // Drag & drop
        //--------------------------------------------------------------------------
        case WM_DROPFILES:
        {
            HDROP drop = (HDROP)wParam;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);

            if (CORE.window.dropFilepaths != NULL)
            {
                for (unsigned int i = 0; i < CORE.window.dropFileCount; i++) RL_FREE(CORE.window.dropFilepaths[i]);
                RL_FREE(CORE.window.dropFilepaths);
                CORE.window.dropFilepaths = NULL;
                CORE.window.dropFileCount = 0;
            }

            if (count > MAX_DROPPED_FILES) count = MAX_DROPPED_FILES;

            if (count > 0)
            {
                CORE.window.dropFilepaths = (char **)RL_CALLOC(count, sizeof(char *));
                if (CORE.window.dropFilepaths != NULL)
                {
                    unsigned int stored = 0;
                    for (UINT i = 0; i < count; i++)
                    {
                        WCHAR widePath[MAX_FILEPATH_LENGTH];
                        widePath[0] = 0;
                        if (DragQueryFileW(drop, i, widePath, MAX_FILEPATH_LENGTH) == 0) continue;

                        char *path = (char *)RL_CALLOC(MAX_FILEPATH_LENGTH, 1);
                        if (path == NULL) continue;

                        if (WideCharToMultiByte(CP_UTF8, 0, widePath, -1, path, MAX_FILEPATH_LENGTH, NULL, NULL) <= 0)
                        {
                            RL_FREE(path);
                            continue;
                        }

                        CORE.window.dropFilepaths[stored] = path;
                        stored++;
                    }
                    CORE.window.dropFileCount = stored;
                }
            }

            DragFinish(drop);
            TRACELOG(LOG_INFO, "PLATFORM: Dropped %u file(s) on window", CORE.window.dropFileCount);
            return 0;
        }
        default: break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//----------------------------------------------------------------------------------
// Vulkan surface glue
//----------------------------------------------------------------------------------
static uint64_t CreateSurfaceWin32(void *vkInstance, void *userData)
{
    (void)userData;

    VkInstance instance = (VkInstance)vkInstance;
    if ((instance == VK_NULL_HANDLE) || (platform.handle == NULL)) return 0;

    PFN_vkCreateWin32SurfaceKHR createSurface =
        (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
    if (createSurface == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: vkCreateWin32SurfaceKHR not available");
        return 0;
    }

    VkWin32SurfaceCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .pNext = NULL,
        .flags = 0,
        .hinstance = platform.instance,
        .hwnd = platform.handle
    };

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (createSurface(instance, &createInfo, NULL, &surface) != VK_SUCCESS)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create Vulkan Win32 surface");
        return 0;
    }

#if defined(VK_USE_64_BIT_PTR_DEFINES) && (VK_USE_64_BIT_PTR_DEFINES == 1)
    return (uint64_t)(uintptr_t)surface;
#else
    return (uint64_t)surface;
#endif
}

//----------------------------------------------------------------------------------
// Gamepads (XInput, dynamically loaded)
//----------------------------------------------------------------------------------
static void LoadXInput(void)
{
    static const WCHAR *libraries[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };

    for (int i = 0; (i < 3) && (platform.xinputLib == NULL); i++)
    {
        platform.xinputLib = LoadLibraryW(libraries[i]);
    }

    if (platform.xinputLib == NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: XInput library not available, gamepads disabled");
        return;
    }

    platform.XInputGetStateFn = (PFN_XInputGetStateProc)(void (*)(void))GetProcAddress(platform.xinputLib, "XInputGetState");
    platform.XInputSetStateFn = (PFN_XInputSetStateProc)(void (*)(void))GetProcAddress(platform.xinputLib, "XInputSetState");

    if (platform.XInputGetStateFn == NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: XInput entry points missing, gamepads disabled");
        FreeLibrary(platform.xinputLib);
        platform.xinputLib = NULL;
    }
    else TRACELOG(LOG_INFO, "PLATFORM: XInput initialized (gamepad support)");
}

// Map an XInput trigger byte to the raylib [-1..1] axis range
static float TriggerToAxis(BYTE value)
{
    return ((float)value/255.0f)*2.0f - 1.0f;
}

// Map an XInput thumbstick value to [-1..1]
static float ThumbToAxis(SHORT value)
{
    float result = (float)value/32767.0f;
    if (result < -1.0f) result = -1.0f;
    if (result > 1.0f) result = 1.0f;
    return result;
}

static void UpdateGamepads(void)
{
    if (platform.XInputGetStateFn == NULL) return;

    platform.gamepadRescanCounter++;
    bool rescan = ((platform.gamepadRescanCounter % 120) == 0);
    double now = GetTimePlatform();

    for (int i = 0; i < MAX_GAMEPADS; i++)
    {
        // Querying a disconnected slot is expensive, only retry periodically
        if (!CORE.input.gamepad.ready[i] && !rescan) continue;

        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        if (platform.XInputGetStateFn((DWORD)i, &state) != ERROR_SUCCESS)
        {
            if (CORE.input.gamepad.ready[i])
            {
                CORE.input.gamepad.ready[i] = false;
                CORE.input.gamepad.axisCount[i] = 0;
                CORE.input.gamepad.name[i][0] = '\0';
                for (int b = 0; b < MAX_GAMEPAD_BUTTONS; b++) CORE.input.gamepad.currentButtonState[i][b] = 0;
                for (int a = 0; a < MAX_GAMEPAD_AXES; a++) CORE.input.gamepad.axisState[i][a] = 0.0f;
                TRACELOG(LOG_INFO, "PLATFORM: Gamepad %i disconnected", i);
            }
            continue;
        }

        if (!CORE.input.gamepad.ready[i])
        {
            CORE.input.gamepad.ready[i] = true;
            CORE.input.gamepad.axisCount[i] = 6;
            TextCopy(CORE.input.gamepad.name[i], "Xbox Controller (XInput)");
            TRACELOG(LOG_INFO, "PLATFORM: Gamepad %i connected: %s", i, CORE.input.gamepad.name[i]);
        }

        // Vibration timeout
        if ((platform.vibrationEnd[i] > 0.0) && (now >= platform.vibrationEnd[i]))
        {
            platform.vibrationEnd[i] = 0.0;
            if (platform.XInputSetStateFn != NULL)
            {
                XINPUT_VIBRATION stop;
                memset(&stop, 0, sizeof(stop));
                platform.XInputSetStateFn((DWORD)i, &stop);
            }
        }

        const WORD buttons = state.Gamepad.wButtons;
        char *current = CORE.input.gamepad.currentButtonState[i];

        current[GAMEPAD_BUTTON_LEFT_FACE_UP] = ((buttons & XINPUT_GAMEPAD_DPAD_UP) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_LEFT_FACE_RIGHT] = ((buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_LEFT_FACE_DOWN] = ((buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_LEFT_FACE_LEFT] = ((buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_RIGHT_FACE_UP] = ((buttons & XINPUT_GAMEPAD_Y) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_RIGHT_FACE_RIGHT] = ((buttons & XINPUT_GAMEPAD_B) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_RIGHT_FACE_DOWN] = ((buttons & XINPUT_GAMEPAD_A) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_RIGHT_FACE_LEFT] = ((buttons & XINPUT_GAMEPAD_X) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_LEFT_TRIGGER_1] = ((buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_RIGHT_TRIGGER_1] = ((buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_MIDDLE_LEFT] = ((buttons & XINPUT_GAMEPAD_BACK) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_MIDDLE_RIGHT] = ((buttons & XINPUT_GAMEPAD_START) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_LEFT_THUMB] = ((buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_RIGHT_THUMB] = ((buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0)? 1 : 0;
        current[GAMEPAD_BUTTON_LEFT_TRIGGER_2] = (state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)? 1 : 0;
        current[GAMEPAD_BUTTON_RIGHT_TRIGGER_2] = (state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)? 1 : 0;
        current[GAMEPAD_BUTTON_MIDDLE] = 0;     // XInput does not expose the guide button publicly

        for (int b = 0; b < MAX_GAMEPAD_BUTTONS; b++)
        {
            if (current[b] == 1) CORE.input.gamepad.lastButtonPressed = b;
        }

        CORE.input.gamepad.axisState[i][GAMEPAD_AXIS_LEFT_X] = ThumbToAxis(state.Gamepad.sThumbLX);
        CORE.input.gamepad.axisState[i][GAMEPAD_AXIS_LEFT_Y] = -ThumbToAxis(state.Gamepad.sThumbLY);
        CORE.input.gamepad.axisState[i][GAMEPAD_AXIS_RIGHT_X] = ThumbToAxis(state.Gamepad.sThumbRX);
        CORE.input.gamepad.axisState[i][GAMEPAD_AXIS_RIGHT_Y] = -ThumbToAxis(state.Gamepad.sThumbRY);
        CORE.input.gamepad.axisState[i][GAMEPAD_AXIS_LEFT_TRIGGER] = TriggerToAxis(state.Gamepad.bLeftTrigger);
        CORE.input.gamepad.axisState[i][GAMEPAD_AXIS_RIGHT_TRIGGER] = TriggerToAxis(state.Gamepad.bRightTrigger);
    }
}

//----------------------------------------------------------------------------------
// Icons
//----------------------------------------------------------------------------------

// Build an HICON from a raylib Image (any uncompressed format, converted to RGBA)
// image.data nonnull, desiredWidth/Height > 0. Returns NULL on failure.
static HICON CreateIconFromImage(Image image, int desiredWidth, int desiredHeight, bool isCursorUnused)
{
    (void)isCursorUnused;

    if ((image.data == NULL) || (image.width <= 0) || (image.height <= 0)) return NULL;

    Image rgba = image;
    bool ownsData = false;
    if (image.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
    {
        rgba = ImageCopy(image);
        if (rgba.data == NULL) return NULL;
        ImageFormat(&rgba, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        ownsData = true;
        if (rgba.data == NULL) return NULL;
    }

    const int width = rgba.width;
    const int height = rgba.height;
    const unsigned char *pixels = (const unsigned char *)rgba.data;

    // RT_ICON resource layout: BITMAPINFOHEADER (height doubled) + BGRA XOR bits + AND mask
    const int maskStride = ((width + 31)/32)*4;
    const size_t colorSize = (size_t)width*(size_t)height*4;
    const size_t maskSize = (size_t)maskStride*(size_t)height;
    const size_t totalSize = sizeof(BITMAPINFOHEADER) + colorSize + maskSize;

    unsigned char *resource = (unsigned char *)RL_CALLOC(totalSize, 1);
    if (resource == NULL)
    {
        if (ownsData) UnloadImage(rgba);
        return NULL;
    }

    BITMAPINFOHEADER header;
    memset(&header, 0, sizeof(header));
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = width;
    header.biHeight = height*2;         // XOR + AND stacked
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = (DWORD)(colorSize + maskSize);
    memcpy(resource, &header, sizeof(header));

    unsigned char *color = resource + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < height; y++)
    {
        const unsigned char *src = pixels + (size_t)(height - 1 - y)*(size_t)width*4;    // Bottom-up
        unsigned char *dst = color + (size_t)y*(size_t)width*4;
        for (int x = 0; x < width; x++)
        {
            dst[x*4 + 0] = src[x*4 + 2];    // B
            dst[x*4 + 1] = src[x*4 + 1];    // G
            dst[x*4 + 2] = src[x*4 + 0];    // R
            dst[x*4 + 3] = src[x*4 + 3];    // A
        }
    }
    // AND mask stays zeroed: alpha channel drives transparency for 32bpp icons

    HICON icon = CreateIconFromResourceEx(resource, (DWORD)totalSize, TRUE, 0x00030000,
                                          desiredWidth, desiredHeight, LR_DEFAULTCOLOR);

    RL_FREE(resource);
    if (ownsData) UnloadImage(rgba);

    if (icon == NULL) TRACELOG(LOG_WARNING, "PLATFORM: Failed to create window icon from image");

    return icon;
}

//----------------------------------------------------------------------------------
// Platform contract: initialization
//----------------------------------------------------------------------------------
int InitPlatform(void)
{
    memset(&platform, 0, sizeof(platform));

    platform.instance = GetModuleHandleW(NULL);
    QueryPerformanceFrequency(&platform.timerFrequency);
    if (platform.timerFrequency.QuadPart == 0) platform.timerFrequency.QuadPart = 1;

    // Resolve optional per-monitor DPI entry points (Windows 10 1607+)
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != NULL)
    {
        platform.GetDpiForWindowFn = (PFN_GetDpiForWindow)(void (*)(void))GetProcAddress(user32, "GetDpiForWindow");
        platform.AdjustWindowRectExForDpiFn =
            (PFN_AdjustWindowRectExForDpi)(void (*)(void))GetProcAddress(user32, "AdjustWindowRectExForDpi");

        if ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0)
        {
            PFN_SetProcessDpiAwarenessContext setContext =
                (PFN_SetProcessDpiAwarenessContext)(void (*)(void))GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (setContext != NULL) setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            else TRACELOG(LOG_WARNING, "PLATFORM: Per-monitor DPI awareness not available on this system");
        }
    }

    // Window class
    WNDCLASSEXW windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.style = CS_HREDRAW|CS_VREDRAW|CS_OWNDC;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = platform.instance;
    windowClass.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    windowClass.hCursor = NULL;                     // Handled from WM_SETCURSOR
    windowClass.hbrBackground = NULL;
    windowClass.lpszClassName = RAYVK_WNDCLASS_NAME;

    if (RegisterClassExW(&windowClass) == 0)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to register window class");
        return -1;
    }
    platform.classRegistered = true;

    platform.cursors[MOUSE_CURSOR_DEFAULT] = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    platform.currentCursor = platform.cursors[MOUSE_CURSOR_DEFAULT];

    RefreshMonitors();

    // Requested client size (screen size is logical, window is created at 100% and
    // corrected by WM_DPICHANGED / UpdateWindowMetrics afterwards)
    if (CORE.window.screen.width <= 0) CORE.window.screen.width = 800;
    if (CORE.window.screen.height <= 0) CORE.window.screen.height = 450;
    CORE.window.scale.x = 1.0f;
    CORE.window.scale.y = 1.0f;

    platform.style = BuildWindowStyle(CORE.window.flags);
    platform.styleEx = BuildWindowStyleEx(CORE.window.flags);

    RECT rect = { 0, 0, (LONG)CORE.window.screen.width, (LONG)CORE.window.screen.height };
    AdjustWindowRectEx(&rect, platform.style, FALSE, platform.styleEx);
    int windowWidth = (int)(rect.right - rect.left);
    int windowHeight = (int)(rect.bottom - rect.top);

    // Center on the primary monitor work area unless a position was requested
    int posX = CW_USEDEFAULT;
    int posY = CW_USEDEFAULT;
    if ((CORE.window.position.x != 0) || (CORE.window.position.y != 0))
    {
        posX = CORE.window.position.x;
        posY = CORE.window.position.y;
    }
    else
    {
        RECT work = { 0, 0, 0, 0 };
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        {
            posX = (int)(work.left + ((work.right - work.left) - windowWidth)/2);
            posY = (int)(work.top + ((work.bottom - work.top) - windowHeight)/2);
        }
    }

    WCHAR *wideTitle = WideFromUtf8((CORE.window.title != NULL)? CORE.window.title : "rayvulkan");
    platform.handle = CreateWindowExW(platform.styleEx, RAYVK_WNDCLASS_NAME,
                                      (wideTitle != NULL)? wideTitle : L"rayvulkan",
                                      platform.style, posX, posY, windowWidth, windowHeight,
                                      NULL, NULL, platform.instance, NULL);
    if (wideTitle != NULL) RL_FREE(wideTitle);

    if (platform.handle == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create window");
        UnregisterClassW(RAYVK_WNDCLASS_NAME, platform.instance);
        platform.classRegistered = false;
        return -1;
    }

    DragAcceptFiles(platform.handle, TRUE);

    if ((CORE.window.flags & FLAG_WINDOW_TRANSPARENT) > 0)
    {
        // A layered window can be made translucent, but a per-pixel transparent Vulkan
        // swapchain is not available through VK_KHR_win32_surface
        SetLayeredWindowAttributes(platform.handle, 0, 255, LWA_ALPHA);
        TRACELOG(LOG_WARNING, "PLATFORM: FLAG_WINDOW_TRANSPARENT: per-pixel framebuffer transparency unsupported on Win32/Vulkan");
    }

    UpdateWindowMetrics();

    // Fullscreen requested from the start
    if ((CORE.window.flags & FLAG_FULLSCREEN_MODE) > 0)
    {
        CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;      // ToggleFullscreenPlatform sets it back
        ToggleFullscreenPlatform();
    }
    else if ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0)
    {
        CORE.window.flags &= ~FLAG_BORDERLESS_WINDOWED_MODE;
        ToggleBorderlessWindowedPlatform();
    }

    if ((CORE.window.flags & FLAG_WINDOW_HIDDEN) == 0)
    {
        int showCommand = SW_SHOWNORMAL;
        if ((CORE.window.flags & FLAG_WINDOW_MINIMIZED) > 0) showCommand = SW_SHOWMINIMIZED;
        else if ((CORE.window.flags & FLAG_WINDOW_MAXIMIZED) > 0) showCommand = SW_SHOWMAXIMIZED;

        ShowWindow(platform.handle, showCommand);
        UpdateWindow(platform.handle);

        if ((CORE.window.flags & FLAG_WINDOW_UNFOCUSED) == 0) SetForegroundWindow(platform.handle);
    }

    UpdateWindowMetrics();

    // Display size of the monitor holding the window
    RefreshMonitors();
    int monitor = GetCurrentMonitorPlatform();
    if (MonitorIsValid(monitor))
    {
        CORE.window.display.width = (int)(platform.monitors[monitor].bounds.right - platform.monitors[monitor].bounds.left);
        CORE.window.display.height = (int)(platform.monitors[monitor].bounds.bottom - platform.monitors[monitor].bounds.top);
    }

    // High resolution timer for WaitTime()
    if (timeBeginPeriod(1) == TIMERR_NOERROR) platform.timerPeriodSet = true;

    LoadXInput();

    // Vulkan initialization
    static const char *instanceExtensions[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    rlvkPlatformGlue glue = {
        .requiredExtensions = instanceExtensions,
        .requiredExtensionCount = 2,
        .createSurface = CreateSurfaceWin32,
        .userData = NULL
    };

    if (!rlvkInit(&glue, CORE.window.render.width, CORE.window.render.height,
                  ((CORE.window.flags & FLAG_VSYNC_HINT) > 0)))
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to initialize Vulkan (rlvk)");
        ClosePlatform();
        return -1;
    }

    CORE.input.mouse.cursorOnScreen = true;
    CORE.input.mouse.scale.x = 1.0f;
    CORE.input.mouse.scale.y = 1.0f;

    TRACELOG(LOG_INFO, "PLATFORM: Initialized successfully (Win32)");
    TRACELOG(LOG_INFO, "    > Window size:   %i x %i", CORE.window.screen.width, CORE.window.screen.height);
    TRACELOG(LOG_INFO, "    > Render size:   %i x %i", CORE.window.render.width, CORE.window.render.height);
    TRACELOG(LOG_INFO, "    > Display size:  %i x %i", CORE.window.display.width, CORE.window.display.height);
    TRACELOG(LOG_INFO, "    > Monitors:      %i", platform.monitorCount);

    return 0;
}

void ClosePlatform(void)
{
    if (CORE.input.mouse.cursorLocked)
    {
        ClipCursorToWindow(false);
        SetRawMouseInput(false);
        CORE.input.mouse.cursorLocked = false;
    }

    if (platform.bigIcon != NULL) { DestroyIcon(platform.bigIcon); platform.bigIcon = NULL; }
    if (platform.smallIcon != NULL) { DestroyIcon(platform.smallIcon); platform.smallIcon = NULL; }

    if (platform.handle != NULL)
    {
        DragAcceptFiles(platform.handle, FALSE);
        DestroyWindow(platform.handle);
        platform.handle = NULL;
    }

    if (platform.classRegistered)
    {
        UnregisterClassW(RAYVK_WNDCLASS_NAME, platform.instance);
        platform.classRegistered = false;
    }

    if (platform.timerPeriodSet)
    {
        timeEndPeriod(1);
        platform.timerPeriodSet = false;
    }

    if (platform.xinputLib != NULL)
    {
        FreeLibrary(platform.xinputLib);
        platform.xinputLib = NULL;
        platform.XInputGetStateFn = NULL;
        platform.XInputSetStateFn = NULL;
    }

    if (platform.clipboardText != NULL)
    {
        RL_FREE(platform.clipboardText);
        platform.clipboardText = NULL;
    }

    TRACELOG(LOG_INFO, "PLATFORM: Closed (Win32)");
}

//----------------------------------------------------------------------------------
// Platform contract: event pump and timing
//----------------------------------------------------------------------------------
void PollInputEventsPlatform(void)
{
    // Per-frame input bookkeeping (mirrors raylib platform layers)
    CORE.input.keyboard.keyPressedQueueCount = 0;
    CORE.input.keyboard.charPressedQueueCount = 0;

    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++)
    {
        CORE.input.keyboard.previousKeyState[i] = CORE.input.keyboard.currentKeyState[i];
        CORE.input.keyboard.keyRepeatInFrame[i] = 0;
    }

    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) CORE.input.mouse.previousButtonState[i] = CORE.input.mouse.currentButtonState[i];
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.input.touch.previousTouchState[i] = CORE.input.touch.currentTouchState[i];

    for (int i = 0; i < MAX_GAMEPADS; i++)
    {
        for (int b = 0; b < MAX_GAMEPAD_BUTTONS; b++)
        {
            CORE.input.gamepad.previousButtonState[i][b] = CORE.input.gamepad.currentButtonState[i][b];
        }
    }

    CORE.input.mouse.previousWheelMove = CORE.input.mouse.currentWheelMove;
    CORE.input.mouse.currentWheelMove.x = 0.0f;
    CORE.input.mouse.currentWheelMove.y = 0.0f;
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;

    CORE.window.resizedLastFrame = false;

    // Native events
    if (CORE.window.eventWaiting && !CORE.window.shouldClose) WaitMessage();

    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            CORE.window.shouldClose = true;
            break;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UpdateGamepads();
}

double GetTimePlatform(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    LONGLONG frequency = platform.timerFrequency.QuadPart;
    if (frequency == 0) frequency = 1;

    return (double)counter.QuadPart/(double)frequency;
}

// Sleep most of the requested time, then busy-wait for the remainder (sub-ms accuracy)
void WaitTimePlatform(double seconds)
{
    if (seconds <= 0.0) return;

    double destination = GetTimePlatform() + seconds;

    // timeBeginPeriod(1) makes Sleep() granularity ~1ms; keep a 2ms safety margin
    double sleepSeconds = seconds - 0.002;
    if (sleepSeconds > 0.0) Sleep((DWORD)(sleepSeconds*1000.0));

    while (GetTimePlatform() < destination) { /* busy wait for the last fraction */ }
}

void SwapScreenBufferPlatform(void)
{
    rlvkEndFrame();
}

//----------------------------------------------------------------------------------
// Platform contract: window control
//----------------------------------------------------------------------------------
void SetWindowTitlePlatform(const char *title)
{
    if (platform.handle == NULL) return;

    CORE.window.title = title;

    WCHAR *wide = WideFromUtf8((title != NULL)? title : "");
    if (wide == NULL) return;

    SetWindowTextW(platform.handle, wide);
    RL_FREE(wide);
}

void SetWindowPositionPlatform(int x, int y)
{
    if (platform.handle == NULL) return;

    SetWindowPos(platform.handle, NULL, x, y, 0, 0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    CORE.window.position.x = x;
    CORE.window.position.y = y;
}

void SetWindowSizePlatform(int width, int height)
{
    if ((platform.handle == NULL) || (width <= 0) || (height <= 0)) return;

    float scale = ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0)? CORE.window.scale.x : 1.0f;
    if (scale <= 0.0f) scale = 1.0f;

    RECT rect = { 0, 0, (LONG)(width*scale), (LONG)(height*scale) };
    AdjustRectForWindow(&rect);

    SetWindowPos(platform.handle, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);

    UpdateWindowMetrics();
}

void SetWindowMinSizePlatform(int width, int height)
{
    CORE.window.screenMin.width = width;
    CORE.window.screenMin.height = height;

    // Force the size constraints to be re-evaluated
    if (platform.handle != NULL) SetWindowPos(platform.handle, NULL, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}

void SetWindowMaxSizePlatform(int width, int height)
{
    CORE.window.screenMax.width = width;
    CORE.window.screenMax.height = height;

    if (platform.handle != NULL) SetWindowPos(platform.handle, NULL, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}

void SetWindowOpacityPlatform(float opacity)
{
    if (platform.handle == NULL) return;

    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;

    LONG_PTR styleEx = GetWindowLongPtrW(platform.handle, GWL_EXSTYLE);
    if ((styleEx & WS_EX_LAYERED) == 0)
    {
        SetWindowLongPtrW(platform.handle, GWL_EXSTYLE, styleEx|WS_EX_LAYERED);
        platform.styleEx |= WS_EX_LAYERED;
    }

    if (!SetLayeredWindowAttributes(platform.handle, 0, (BYTE)(opacity*255.0f), LWA_ALPHA))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to set window opacity");
    }
}

void SetWindowIconPlatform(Image *images, int count)
{
    if (platform.handle == NULL) return;

    HICON previousBig = platform.bigIcon;
    HICON previousSmall = platform.smallIcon;
    platform.bigIcon = NULL;
    platform.smallIcon = NULL;

    if ((images != NULL) && (count > 0))
    {
        const int bigWidth = GetSystemMetrics(SM_CXICON);
        const int bigHeight = GetSystemMetrics(SM_CYICON);
        const int smallWidth = GetSystemMetrics(SM_CXSMICON);
        const int smallHeight = GetSystemMetrics(SM_CYSMICON);

        // Pick the closest image for each requested size (deterministic: first best wins)
        int bigIndex = 0;
        int smallIndex = 0;
        long bigDelta = -1;
        long smallDelta = -1;

        for (int i = 0; i < count; i++)
        {
            if (images[i].data == NULL) continue;

            long deltaBig = labs((long)images[i].width - (long)bigWidth) + labs((long)images[i].height - (long)bigHeight);
            long deltaSmall = labs((long)images[i].width - (long)smallWidth) + labs((long)images[i].height - (long)smallHeight);

            if ((bigDelta < 0) || (deltaBig < bigDelta)) { bigDelta = deltaBig; bigIndex = i; }
            if ((smallDelta < 0) || (deltaSmall < smallDelta)) { smallDelta = deltaSmall; smallIndex = i; }
        }

        if (bigDelta >= 0) platform.bigIcon = CreateIconFromImage(images[bigIndex], bigWidth, bigHeight, false);
        if (smallDelta >= 0) platform.smallIcon = CreateIconFromImage(images[smallIndex], smallWidth, smallHeight, false);
    }

    SendMessageW(platform.handle, WM_SETICON, ICON_BIG, (LPARAM)platform.bigIcon);
    SendMessageW(platform.handle, WM_SETICON, ICON_SMALL, (LPARAM)platform.smallIcon);

    if (previousBig != NULL) DestroyIcon(previousBig);
    if (previousSmall != NULL) DestroyIcon(previousSmall);
}

void SetWindowFocusedPlatform(void)
{
    if (platform.handle == NULL) return;

    BringWindowToTop(platform.handle);
    SetForegroundWindow(platform.handle);
    SetFocus(platform.handle);
}

void SetWindowMonitorPlatform(int monitor)
{
    if (platform.handle == NULL) return;

    RefreshMonitors();
    if (!MonitorIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return;
    }

    const RECT bounds = platform.monitors[monitor].bounds;
    const int monitorWidth = (int)(bounds.right - bounds.left);
    const int monitorHeight = (int)(bounds.bottom - bounds.top);

    if (CORE.window.fullscreen || ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0))
    {
        SetWindowPos(platform.handle, HWND_TOP, bounds.left, bounds.top, monitorWidth, monitorHeight, SWP_NOACTIVATE);
    }
    else
    {
        RECT window = { 0, 0, 0, 0 };
        GetWindowRect(platform.handle, &window);
        const int windowWidth = (int)(window.right - window.left);
        const int windowHeight = (int)(window.bottom - window.top);

        SetWindowPos(platform.handle, NULL,
                     (int)(bounds.left + (monitorWidth - windowWidth)/2),
                     (int)(bounds.top + (monitorHeight - windowHeight)/2),
                     0, 0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }

    CORE.window.display.width = monitorWidth;
    CORE.window.display.height = monitorHeight;
    UpdateWindowMetrics();
}

void SetWindowStatePlatform(unsigned int flags)
{
    if (platform.handle == NULL) return;

    CORE.window.flags |= flags;

    if ((flags & FLAG_VSYNC_HINT) > 0) rlvkSetVSync(true);
    if ((flags & FLAG_FULLSCREEN_MODE) > 0)
    {
        CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;
        ToggleFullscreenPlatform();
    }
    if ((flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0)
    {
        CORE.window.flags &= ~FLAG_BORDERLESS_WINDOWED_MODE;
        ToggleBorderlessWindowedPlatform();
    }
    if ((flags & (FLAG_WINDOW_RESIZABLE|FLAG_WINDOW_UNDECORATED|FLAG_WINDOW_TOPMOST|
                  FLAG_WINDOW_MOUSE_PASSTHROUGH|FLAG_WINDOW_TRANSPARENT)) > 0) ApplyWindowStyles(false);
    if ((flags & FLAG_WINDOW_HIDDEN) > 0) ShowWindow(platform.handle, SW_HIDE);
    if ((flags & FLAG_WINDOW_MINIMIZED) > 0) ShowWindow(platform.handle, SW_MINIMIZE);
    if ((flags & FLAG_WINDOW_MAXIMIZED) > 0) ShowWindow(platform.handle, SW_MAXIMIZE);
    if ((flags & FLAG_WINDOW_UNFOCUSED) == 0) { /* focus is handled by SetWindowFocused() */ }
    if ((flags & FLAG_MSAA_4X_HINT) > 0) TRACELOG(LOG_WARNING, "PLATFORM: MSAA can only be configured before window creation");
    if ((flags & FLAG_INTERLACED_HINT) > 0) TRACELOG(LOG_WARNING, "PLATFORM: FLAG_INTERLACED_HINT not supported on Win32");
}

void ClearWindowStatePlatform(unsigned int flags)
{
    if (platform.handle == NULL) return;

    if ((flags & FLAG_VSYNC_HINT) > 0) rlvkSetVSync(false);
    if (((flags & FLAG_FULLSCREEN_MODE) > 0) && CORE.window.fullscreen) ToggleFullscreenPlatform();
    if (((flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0) &&
        ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0)) ToggleBorderlessWindowedPlatform();

    CORE.window.flags &= ~flags;

    if ((flags & (FLAG_WINDOW_RESIZABLE|FLAG_WINDOW_UNDECORATED|FLAG_WINDOW_TOPMOST|
                  FLAG_WINDOW_MOUSE_PASSTHROUGH|FLAG_WINDOW_TRANSPARENT)) > 0) ApplyWindowStyles(false);
    if ((flags & FLAG_WINDOW_HIDDEN) > 0) ShowWindow(platform.handle, SW_SHOW);
    if (((flags & FLAG_WINDOW_MINIMIZED) > 0) || ((flags & FLAG_WINDOW_MAXIMIZED) > 0)) ShowWindow(platform.handle, SW_RESTORE);
}

// Enter/leave a WS_POPUP window covering the monitor holding the window
static void ToggleMonitorSpanning(bool enable)
{
    if (platform.handle == NULL) return;

    if (enable)
    {
        memset(&platform.restorePlacement, 0, sizeof(platform.restorePlacement));
        platform.restorePlacement.length = sizeof(WINDOWPLACEMENT);
        platform.placementSaved = (GetWindowPlacement(platform.handle, &platform.restorePlacement) != 0);

        CORE.window.previousPosition.x = CORE.window.position.x;
        CORE.window.previousPosition.y = CORE.window.position.y;
        CORE.window.previousScreen.width = CORE.window.screen.width;
        CORE.window.previousScreen.height = CORE.window.screen.height;

        RefreshMonitors();
        int monitor = GetCurrentMonitorPlatform();
        if (!MonitorIsValid(monitor))
        {
            TRACELOG(LOG_WARNING, "PLATFORM: No monitor available for fullscreen");
            return;
        }

        const RECT bounds = platform.monitors[monitor].bounds;

        platform.style = BuildWindowStyle(CORE.window.flags);
        platform.styleEx = BuildWindowStyleEx(CORE.window.flags);
        SetWindowLongPtrW(platform.handle, GWL_STYLE, (LONG_PTR)(platform.style|WS_VISIBLE));
        SetWindowLongPtrW(platform.handle, GWL_EXSTYLE, (LONG_PTR)platform.styleEx);

        SetWindowPos(platform.handle, HWND_TOP, bounds.left, bounds.top,
                     (int)(bounds.right - bounds.left), (int)(bounds.bottom - bounds.top),
                     SWP_FRAMECHANGED|SWP_NOACTIVATE);

        CORE.window.display.width = (int)(bounds.right - bounds.left);
        CORE.window.display.height = (int)(bounds.bottom - bounds.top);
    }
    else
    {
        platform.style = BuildWindowStyle(CORE.window.flags);
        platform.styleEx = BuildWindowStyleEx(CORE.window.flags);
        SetWindowLongPtrW(platform.handle, GWL_STYLE, (LONG_PTR)(platform.style|WS_VISIBLE));
        SetWindowLongPtrW(platform.handle, GWL_EXSTYLE, (LONG_PTR)platform.styleEx);

        if (platform.placementSaved)
        {
            SetWindowPlacement(platform.handle, &platform.restorePlacement);
            platform.placementSaved = false;
            SetWindowPos(platform.handle, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        else
        {
            RECT rect = { 0, 0, (LONG)CORE.window.previousScreen.width, (LONG)CORE.window.previousScreen.height };
            AdjustRectForWindow(&rect);
            SetWindowPos(platform.handle, NULL, CORE.window.previousPosition.x, CORE.window.previousPosition.y,
                         rect.right - rect.left, rect.bottom - rect.top, SWP_FRAMECHANGED|SWP_NOZORDER|SWP_NOACTIVATE);
        }
    }

    UpdateWindowMetrics();
}

void ToggleFullscreenPlatform(void)
{
    if (platform.handle == NULL) return;

    if (!CORE.window.fullscreen)
    {
        CORE.window.fullscreen = true;
        CORE.window.flags |= FLAG_FULLSCREEN_MODE;
        ToggleMonitorSpanning(true);
        TRACELOG(LOG_INFO, "PLATFORM: Entered fullscreen mode (%i x %i)", CORE.window.render.width, CORE.window.render.height);
    }
    else
    {
        CORE.window.fullscreen = false;
        CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;
        ToggleMonitorSpanning(false);
        TRACELOG(LOG_INFO, "PLATFORM: Left fullscreen mode");
    }
}

void ToggleBorderlessWindowedPlatform(void)
{
    if (platform.handle == NULL) return;

    if ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) == 0)
    {
        CORE.window.flags |= FLAG_BORDERLESS_WINDOWED_MODE;
        ToggleMonitorSpanning(true);
        TRACELOG(LOG_INFO, "PLATFORM: Entered borderless windowed mode");
    }
    else
    {
        CORE.window.flags &= ~FLAG_BORDERLESS_WINDOWED_MODE;
        ToggleMonitorSpanning(false);
        TRACELOG(LOG_INFO, "PLATFORM: Left borderless windowed mode");
    }
}

void MaximizeWindowPlatform(void)
{
    if (platform.handle == NULL) return;

    ShowWindow(platform.handle, SW_MAXIMIZE);
    CORE.window.flags |= FLAG_WINDOW_MAXIMIZED;
}

void MinimizeWindowPlatform(void)
{
    if (platform.handle == NULL) return;

    ShowWindow(platform.handle, SW_MINIMIZE);
    CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
}

void RestoreWindowPlatform(void)
{
    if (platform.handle == NULL) return;

    ShowWindow(platform.handle, SW_RESTORE);
    CORE.window.flags &= ~(FLAG_WINDOW_MINIMIZED|FLAG_WINDOW_MAXIMIZED);
    UpdateWindowMetrics();
}

void *GetWindowHandlePlatform(void)
{
    return (void *)platform.handle;
}

//----------------------------------------------------------------------------------
// Platform contract: monitors
//----------------------------------------------------------------------------------
int GetMonitorCountPlatform(void)
{
    RefreshMonitors();
    return platform.monitorCount;
}

int GetCurrentMonitorPlatform(void)
{
    if (platform.monitorCount == 0) RefreshMonitors();
    if (platform.handle == NULL) return 0;

    HMONITOR current = MonitorFromWindow(platform.handle, MONITOR_DEFAULTTONEAREST);
    return MonitorIndexFromHandle(current);
}

Vector2 GetMonitorPositionPlatform(int monitor)
{
    Vector2 position = { 0.0f, 0.0f };

    RefreshMonitors();
    if (!MonitorIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return position;
    }

    position.x = (float)platform.monitors[monitor].bounds.left;
    position.y = (float)platform.monitors[monitor].bounds.top;

    return position;
}

int GetMonitorWidthPlatform(int monitor)
{
    RefreshMonitors();
    if (!MonitorIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return (int)(platform.monitors[monitor].bounds.right - platform.monitors[monitor].bounds.left);
}

int GetMonitorHeightPlatform(int monitor)
{
    RefreshMonitors();
    if (!MonitorIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return (int)(platform.monitors[monitor].bounds.bottom - platform.monitors[monitor].bounds.top);
}

// Query a physical dimension (HORZSIZE/VERTSIZE, millimetres) of a monitor device context
static int GetMonitorPhysicalSize(int monitor, int capIndex)
{
    RefreshMonitors();
    if (!MonitorIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    HDC dc = CreateDCW(L"DISPLAY", platform.monitors[monitor].deviceName, NULL, NULL);
    if (dc == NULL) return 0;

    int size = GetDeviceCaps(dc, capIndex);
    DeleteDC(dc);

    return size;
}

int GetMonitorPhysicalWidthPlatform(int monitor)
{
    return GetMonitorPhysicalSize(monitor, HORZSIZE);
}

int GetMonitorPhysicalHeightPlatform(int monitor)
{
    return GetMonitorPhysicalSize(monitor, VERTSIZE);
}

int GetMonitorRefreshRatePlatform(int monitor)
{
    RefreshMonitors();
    if (!MonitorIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    DEVMODEW mode;
    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(DEVMODEW);

    if (!EnumDisplaySettingsW(platform.monitors[monitor].deviceName, ENUM_CURRENT_SETTINGS, &mode)) return 0;

    // 0 and 1 both mean "hardware default" for the display driver
    if (mode.dmDisplayFrequency <= 1) return 0;

    return (int)mode.dmDisplayFrequency;
}

const char *GetMonitorNamePlatform(int monitor)
{
    platform.monitorName[0] = '\0';

    RefreshMonitors();
    if (!MonitorIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return platform.monitorName;
    }

    // Prefer the human readable monitor description, fall back to the adapter device name
    DISPLAY_DEVICEW device;
    memset(&device, 0, sizeof(device));
    device.cb = sizeof(DISPLAY_DEVICEW);

    const WCHAR *source = platform.monitors[monitor].deviceName;
    if (EnumDisplayDevicesW(platform.monitors[monitor].deviceName, 0, &device, 0) && (device.DeviceString[0] != 0))
    {
        source = device.DeviceString;
    }

    WideCharToMultiByte(CP_UTF8, 0, source, -1, platform.monitorName, (int)sizeof(platform.monitorName), NULL, NULL);
    platform.monitorName[sizeof(platform.monitorName) - 1] = '\0';

    return platform.monitorName;
}

Vector2 GetWindowScaleDPIPlatform(void)
{
    Vector2 scale = { 1.0f, 1.0f };

    if ((platform.handle != NULL) && (platform.GetDpiForWindowFn != NULL))
    {
        UINT dpi = platform.GetDpiForWindowFn(platform.handle);
        if (dpi > 0)
        {
            scale.x = (float)dpi/(float)USER_DEFAULT_SCREEN_DPI;
            scale.y = scale.x;
        }
    }

    return scale;
}

//----------------------------------------------------------------------------------
// Platform contract: cursor
//----------------------------------------------------------------------------------
void ShowCursorPlatform(void)
{
    CORE.input.mouse.cursorHidden = false;
    SetCursor(platform.currentCursor);
}

void HideCursorPlatform(void)
{
    CORE.input.mouse.cursorHidden = true;
    SetCursor(NULL);
}

void EnableCursorPlatform(void)
{
    if (CORE.input.mouse.cursorLocked)
    {
        SetRawMouseInput(false);
        ClipCursorToWindow(false);
    }

    CORE.input.mouse.cursorLocked = false;
    CORE.input.mouse.cursorHidden = false;
    SetCursor(platform.currentCursor);
}

void DisableCursorPlatform(void)
{
    if (platform.handle == NULL) return;

    CenterCursorInWindow();
    ClipCursorToWindow(true);
    SetRawMouseInput(true);

    CORE.input.mouse.cursorLocked = true;
    CORE.input.mouse.cursorHidden = true;
    SetCursor(NULL);
}

void SetMouseCursorPlatform(int cursor)
{
    if ((cursor < 0) || (cursor > MOUSE_CURSOR_NOT_ALLOWED))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Unsupported mouse cursor shape (%i)", cursor);
        return;
    }

    if (platform.cursors[cursor] == NULL)
    {
        const WCHAR *shape = (const WCHAR *)IDC_ARROW;
        switch (cursor)
        {
            case MOUSE_CURSOR_ARROW: shape = (const WCHAR *)IDC_ARROW; break;
            case MOUSE_CURSOR_IBEAM: shape = (const WCHAR *)IDC_IBEAM; break;
            case MOUSE_CURSOR_CROSSHAIR: shape = (const WCHAR *)IDC_CROSS; break;
            case MOUSE_CURSOR_POINTING_HAND: shape = (const WCHAR *)IDC_HAND; break;
            case MOUSE_CURSOR_RESIZE_EW: shape = (const WCHAR *)IDC_SIZEWE; break;
            case MOUSE_CURSOR_RESIZE_NS: shape = (const WCHAR *)IDC_SIZENS; break;
            case MOUSE_CURSOR_RESIZE_NWSE: shape = (const WCHAR *)IDC_SIZENWSE; break;
            case MOUSE_CURSOR_RESIZE_NESW: shape = (const WCHAR *)IDC_SIZENESW; break;
            case MOUSE_CURSOR_RESIZE_ALL: shape = (const WCHAR *)IDC_SIZEALL; break;
            case MOUSE_CURSOR_NOT_ALLOWED: shape = (const WCHAR *)IDC_NO; break;
            default: shape = (const WCHAR *)IDC_ARROW; break;
        }

        platform.cursors[cursor] = LoadCursorW(NULL, (LPCWSTR)shape);
        if (platform.cursors[cursor] == NULL)
        {
            TRACELOG(LOG_WARNING, "PLATFORM: Failed to load system cursor (%i)", cursor);
            return;
        }
    }

    CORE.input.mouse.cursor = cursor;
    platform.currentCursor = platform.cursors[cursor];
    if (!CORE.input.mouse.cursorHidden) SetCursor(platform.currentCursor);
}

void SetMousePositionPlatform(int x, int y)
{
    if (platform.handle == NULL) return;

    CORE.input.mouse.currentPosition.x = (float)x;
    CORE.input.mouse.currentPosition.y = (float)y;
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;

    float scale = ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0)? CORE.window.scale.x : 1.0f;
    if (scale <= 0.0f) scale = 1.0f;

    POINT point = { (LONG)(x*scale), (LONG)(y*scale) };
    if (ClientToScreen(platform.handle, &point)) SetCursorPos(point.x, point.y);
}

//----------------------------------------------------------------------------------
// Platform contract: clipboard
//----------------------------------------------------------------------------------
void SetClipboardTextPlatform(const char *text)
{
    if ((platform.handle == NULL) || (text == NULL)) return;

    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (count <= 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to convert clipboard text to UTF-16");
        return;
    }

    HANDLE memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)count*sizeof(WCHAR));
    if (memory == NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to allocate clipboard memory");
        return;
    }

    WCHAR *buffer = (WCHAR *)GlobalLock(memory);
    if (buffer == NULL)
    {
        GlobalFree(memory);
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to lock clipboard memory");
        return;
    }

    MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, count);
    GlobalUnlock(memory);

    if (!OpenClipboard(platform.handle))
    {
        GlobalFree(memory);
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to open clipboard");
        return;
    }

    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == NULL)
    {
        GlobalFree(memory);                 // Ownership stays with us on failure
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to write clipboard text");
    }

    CloseClipboard();
}

const char *GetClipboardTextPlatform(void)
{
    if (platform.clipboardText != NULL)
    {
        RL_FREE(platform.clipboardText);
        platform.clipboardText = NULL;
    }

    if (platform.handle == NULL) return NULL;

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return NULL;
    if (!OpenClipboard(platform.handle))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to open clipboard");
        return NULL;
    }

    HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    if (memory != NULL)
    {
        const WCHAR *wide = (const WCHAR *)GlobalLock(memory);
        if (wide != NULL)
        {
            platform.clipboardText = Utf8FromWide(wide);
            GlobalUnlock(memory);
        }
    }

    CloseClipboard();

    if (platform.clipboardText == NULL) TRACELOG(LOG_WARNING, "PLATFORM: Clipboard does not contain readable text");

    return platform.clipboardText;
}

Image GetClipboardImagePlatform(void)
{
    Image image = { 0 };

    unsigned int dataSize = 0;
    int width = 0;
    int height = 0;

    unsigned char *fileData = Win32GetClipboardImageData(&width, &height, &dataSize);
    if (fileData == NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Clipboard does not contain an image");
        return image;
    }

    image = LoadImageFromMemory(".bmp", fileData, (int)dataSize);
    RL_FREE(fileData);

    if (image.data == NULL) TRACELOG(LOG_WARNING, "PLATFORM: Failed to decode clipboard image");

    return image;
}

//----------------------------------------------------------------------------------
// Platform contract: misc
//----------------------------------------------------------------------------------
void OpenURLPlatform(const char *url)
{
    if (url == NULL) return;

    // Security: refuse quotes, they allow injecting extra shell arguments
    if (strchr(url, '\"') != NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Provided URL contains invalid characters, not opened");
        return;
    }

    WCHAR *wide = WideFromUtf8(url);
    if (wide == NULL) return;

    HINSTANCE result = ShellExecuteW(NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL);
    RL_FREE(wide);

    // ShellExecuteW returns a value <= 32 on failure
    if ((INT_PTR)result <= 32) TRACELOG(LOG_WARNING, "PLATFORM: Failed to open URL");
}

void SetGamepadVibrationPlatform(int gamepad, float leftMotor, float rightMotor, float duration)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS)) return;
    if (platform.XInputSetStateFn == NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Gamepad vibration not available (XInput missing)");
        return;
    }
    if (!CORE.input.gamepad.ready[gamepad]) return;

    if (leftMotor < 0.0f) leftMotor = 0.0f;
    if (leftMotor > 1.0f) leftMotor = 1.0f;
    if (rightMotor < 0.0f) rightMotor = 0.0f;
    if (rightMotor > 1.0f) rightMotor = 1.0f;
    if (duration < 0.0f) duration = 0.0f;

    XINPUT_VIBRATION vibration;
    memset(&vibration, 0, sizeof(vibration));
    vibration.wLeftMotorSpeed = (WORD)(leftMotor*65535.0f);
    vibration.wRightMotorSpeed = (WORD)(rightMotor*65535.0f);

    if (platform.XInputSetStateFn((DWORD)gamepad, &vibration) != ERROR_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to set gamepad %i vibration", gamepad);
        return;
    }

    platform.vibrationEnd[gamepad] = (duration > 0.0f)? (GetTimePlatform() + (double)duration) : 0.0;
}

int SetGamepadMappingsPlatform(const char *mappings)
{
    (void)mappings;

    // XInput exposes a fixed, already normalized layout: custom mappings are meaningless
    TRACELOG(LOG_WARNING, "PLATFORM: Custom gamepad mappings are not supported by the XInput backend");

    return 0;
}
