/**********************************************************************************************
*
*   rcore_x11 - raylib core platform backend for Linux/X11 (raw Xlib, no GLFW/SDL)
*
*   Implements the platform contract declared in src/rcore_internal.h on top of Xlib,
*   XRandR (monitors), Xcursor (cursor shapes), XFixes (mouse passthrough) and
*   VK_KHR_xlib_surface (Vulkan surface creation through volk).
*
*   CONTRACT NOTES (additions/clarifications over src/rcore_internal.h):
*     - PollInputEventsPlatform() performs the per-frame input bookkeeping itself
*       (previous<-current state copies, queue resets) *before* pumping native events.
*       The operations are idempotent so a caller doing the same beforehand is harmless.
*     - GetWindowHandlePlatform() returns the X11 Window id cast to void*, not a pointer.
*     - Cursor lock (DisableCursor) uses XGrabPointer + warp-to-center delta accumulation.
*
*   FEATURES NOT SUPPORTED BY X11/WMs (logged as warnings): FLAG_INTERLACED_HINT.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#ifndef _GNU_SOURCE
    #define _GNU_SOURCE     // clock_gettime/nanosleep/glob visibility under -std=c99
#endif

#include "rcore_internal.h"
#include "rcore_linux_common.h"

// NOTE: the KEY_* macro collision with <linux/input.h> is neutralized inside
// rcore_linux_common.h (see the #undef block there)

#define VK_USE_PLATFORM_XLIB_KHR
#include "volk.h"

// X11 typedefs `Font` (an XID), which collides with the raylib `Font` struct.
// Rename the X11 one while its headers are parsed, then restore raylib's meaning.
#define Font X11Font
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#undef Font

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

#define MAX_MONITORS            16
#define MAX_XDND_TYPES          16
#define SELECTION_TIMEOUT       1.0     // Seconds waiting for a selection owner to answer
#define LONG_MAX_ITEMS          0x1FFFFFFFL     // Property read length, in 32-bit words

//----------------------------------------------------------------------------------
// Types and global platform state
//----------------------------------------------------------------------------------
typedef struct MonitorInfo {
    int x, y;
    int width, height;
    int physicalWidth, physicalHeight;      // Millimeters
    int refreshRate;                        // Hz
    char name[64];
} MonitorInfo;

typedef struct PlatformData {
    Display *display;
    int screen;
    Window root;
    Window handle;
    Visual *visual;
    Colormap colormap;
    int depth;

    XIM xim;
    XIC xic;

    // Atoms
    Atom wmProtocols, wmDeleteWindow, wmTakeFocus, wmState, netWmState, netWmStateFullscreen;
    Atom netWmStateMaximizedVert, netWmStateMaximizedHorz, netWmStateHidden, netWmStateAbove;
    Atom netWmName, netWmIconName, netWmIcon, netWmPid, netWmWindowOpacity, netActiveWindow;
    Atom motifWmHints, utf8String, targets, multiple, incr, clipboard, rayvkSelection;
    Atom textUriList, imagePng, atomText, atomTimestamp;
    Atom xdndAware, xdndEnter, xdndPosition, xdndStatus, xdndDrop, xdndFinished;
    Atom xdndSelection, xdndActionCopy, xdndTypeList;

    Cursor cursors[MOUSE_CURSOR_NOT_ALLOWED + 1];
    Cursor blankCursor;

    char *clipboardOwnedText;       // Text we advertise as CLIPBOARD owner
    char *clipboardFetched;         // Buffer returned by GetClipboardTextPlatform()

    bool randrAvailable;
    bool xfixesAvailable;

    // Cursor lock state
    bool cursorGrabbed;
    int lockCenterX, lockCenterY;

    // Xdnd state
    Window xdndSource;
    int xdndVersion;
    Atom xdndFormat;
    Time xdndTimestamp;

    bool windowMapped;
} PlatformData;

static PlatformData platform = { 0 };

//----------------------------------------------------------------------------------
// Module internal functions declaration
//----------------------------------------------------------------------------------
static int InitAtoms(void);
static int TranslateKeysym(KeySym sym);
static void ProcessEvent(XEvent *event);
static void UpdateWindowSize(int width, int height);
static void SendStateMessage(Atom state1, Atom state2, int action);
static void ApplyMotifDecorations(bool decorated);
static void ApplySizeHints(void);
static void CenterCursorForLock(void);
static int GetMonitors(MonitorInfo *monitors, int maxCount);
static float GetDisplayScale(void);
static unsigned char *ReadSelection(Atom selection, Atom target, unsigned long *outSize);
static void HandleSelectionRequest(const XSelectionRequestEvent *request);
static void HandleXdndClientMessage(const XClientMessageEvent *message);
static void HandleXdndSelection(const XSelectionEvent *selection);
static void PushCharsUtf8(const char *text, int length);
static uint64_t CreateSurfaceX11(void *instance, void *userData);

//----------------------------------------------------------------------------------
// Small helpers
//----------------------------------------------------------------------------------

// Clamp helper used for window geometry
static inline int MaxInt(int a, int b) { return (a > b)? a : b; }

// True when the window is in a state where EWMH state messages make sense
static inline bool WindowValid(void) { return (platform.display != NULL) && (platform.handle != 0); }

// Decode one UTF-8 codepoint; returns the codepoint and advances *offset, -1 on bad input
static int DecodeCodepoint(const char *text, int length, int *offset)
{
    int i = *offset;
    if (i >= length) return -1;

    unsigned char c = (unsigned char)text[i];
    int extra = 0;
    int codepoint = 0;

    if (c < 0x80) { *offset = i + 1; return c; }
    else if ((c & 0xE0) == 0xC0) { codepoint = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { codepoint = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { codepoint = c & 0x07; extra = 3; }
    else { *offset = i + 1; return -1; }

    if ((i + extra) >= length) { *offset = length; return -1; }

    for (int k = 1; k <= extra; k++)
    {
        unsigned char cc = (unsigned char)text[i + k];
        if ((cc & 0xC0) != 0x80) { *offset = i + k; return -1; }
        codepoint = (codepoint << 6) | (cc & 0x3F);
    }

    *offset = i + extra + 1;
    return codepoint;
}

// Push every codepoint of a UTF-8 buffer into the raylib char queue
static void PushCharsUtf8(const char *text, int length)
{
    int offset = 0;
    while (offset < length)
    {
        int codepoint = DecodeCodepoint(text, length, &offset);
        if (codepoint <= 0) continue;
        if (CORE.input.keyboard.charPressedQueueCount >= MAX_CHAR_PRESSED_QUEUE) break;
        CORE.input.keyboard.charPressedQueue[CORE.input.keyboard.charPressedQueueCount] = codepoint;
        CORE.input.keyboard.charPressedQueueCount++;
    }
}

//----------------------------------------------------------------------------------
// Keyboard mapping
//----------------------------------------------------------------------------------

// Translate an X11 keysym to a raylib KeyboardKey, KEY_NULL when unmapped
static int TranslateKeysym(KeySym sym)
{
    if ((sym >= XK_a) && (sym <= XK_z)) return KEY_A + (int)(sym - XK_a);
    if ((sym >= XK_A) && (sym <= XK_Z)) return KEY_A + (int)(sym - XK_A);
    if ((sym >= XK_0) && (sym <= XK_9)) return KEY_ZERO + (int)(sym - XK_0);
    if ((sym >= XK_F1) && (sym <= XK_F12)) return KEY_F1 + (int)(sym - XK_F1);
    if ((sym >= XK_KP_0) && (sym <= XK_KP_9)) return KEY_KP_0 + (int)(sym - XK_KP_0);

    switch (sym)
    {
        case XK_space: return KEY_SPACE;
        case XK_apostrophe: return KEY_APOSTROPHE;
        case XK_comma: return KEY_COMMA;
        case XK_minus: return KEY_MINUS;
        case XK_period: return KEY_PERIOD;
        case XK_slash: return KEY_SLASH;
        case XK_semicolon: return KEY_SEMICOLON;
        case XK_equal: return KEY_EQUAL;
        case XK_bracketleft: return KEY_LEFT_BRACKET;
        case XK_backslash: return KEY_BACKSLASH;
        case XK_bracketright: return KEY_RIGHT_BRACKET;
        case XK_grave: return KEY_GRAVE;
        case XK_Escape: return KEY_ESCAPE;
        case XK_Return: return KEY_ENTER;
        case XK_Tab: case XK_ISO_Left_Tab: return KEY_TAB;
        case XK_BackSpace: return KEY_BACKSPACE;
        case XK_Insert: return KEY_INSERT;
        case XK_Delete: return KEY_DELETE;
        case XK_Right: return KEY_RIGHT;
        case XK_Left: return KEY_LEFT;
        case XK_Down: return KEY_DOWN;
        case XK_Up: return KEY_UP;
        case XK_Prior: return KEY_PAGE_UP;
        case XK_Next: return KEY_PAGE_DOWN;
        case XK_Home: return KEY_HOME;
        case XK_End: return KEY_END;
        case XK_Caps_Lock: return KEY_CAPS_LOCK;
        case XK_Scroll_Lock: return KEY_SCROLL_LOCK;
        case XK_Num_Lock: return KEY_NUM_LOCK;
        case XK_Print: return KEY_PRINT_SCREEN;
        case XK_Pause: return KEY_PAUSE;
        case XK_Shift_L: return KEY_LEFT_SHIFT;
        case XK_Control_L: return KEY_LEFT_CONTROL;
        case XK_Alt_L: return KEY_LEFT_ALT;
        case XK_Super_L: return KEY_LEFT_SUPER;
        case XK_Shift_R: return KEY_RIGHT_SHIFT;
        case XK_Control_R: return KEY_RIGHT_CONTROL;
        case XK_Alt_R: case XK_ISO_Level3_Shift: return KEY_RIGHT_ALT;
        case XK_Super_R: return KEY_RIGHT_SUPER;
        case XK_Menu: return KEY_KB_MENU;
        case XK_KP_Decimal: case XK_KP_Delete: return KEY_KP_DECIMAL;
        case XK_KP_Divide: return KEY_KP_DIVIDE;
        case XK_KP_Multiply: return KEY_KP_MULTIPLY;
        case XK_KP_Subtract: return KEY_KP_SUBTRACT;
        case XK_KP_Add: return KEY_KP_ADD;
        case XK_KP_Enter: return KEY_KP_ENTER;
        case XK_KP_Equal: return KEY_KP_EQUAL;
        case XK_KP_Insert: return KEY_KP_0;
        case XK_KP_End: return KEY_KP_1;
        case XK_KP_Down: return KEY_KP_2;
        case XK_KP_Next: return KEY_KP_3;
        case XK_KP_Left: return KEY_KP_4;
        case XK_KP_Begin: return KEY_KP_5;
        case XK_KP_Right: return KEY_KP_6;
        case XK_KP_Home: return KEY_KP_7;
        case XK_KP_Up: return KEY_KP_8;
        case XK_KP_Prior: return KEY_KP_9;
        default: break;
    }

    return KEY_NULL;
}

//----------------------------------------------------------------------------------
// Atoms
//----------------------------------------------------------------------------------

// Intern every atom used by the backend; returns 0 on success
static int InitAtoms(void)
{
    Display *d = platform.display;

    platform.wmProtocols = XInternAtom(d, "WM_PROTOCOLS", False);
    platform.wmDeleteWindow = XInternAtom(d, "WM_DELETE_WINDOW", False);
    platform.wmTakeFocus = XInternAtom(d, "WM_TAKE_FOCUS", False);
    platform.wmState = XInternAtom(d, "WM_STATE", False);
    platform.netWmState = XInternAtom(d, "_NET_WM_STATE", False);
    platform.netWmStateFullscreen = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    platform.netWmStateMaximizedVert = XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    platform.netWmStateMaximizedHorz = XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    platform.netWmStateHidden = XInternAtom(d, "_NET_WM_STATE_HIDDEN", False);
    platform.netWmStateAbove = XInternAtom(d, "_NET_WM_STATE_ABOVE", False);
    platform.netWmName = XInternAtom(d, "_NET_WM_NAME", False);
    platform.netWmIconName = XInternAtom(d, "_NET_WM_ICON_NAME", False);
    platform.netWmIcon = XInternAtom(d, "_NET_WM_ICON", False);
    platform.netWmPid = XInternAtom(d, "_NET_WM_PID", False);
    platform.netWmWindowOpacity = XInternAtom(d, "_NET_WM_WINDOW_OPACITY", False);
    platform.netActiveWindow = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    platform.motifWmHints = XInternAtom(d, "_MOTIF_WM_HINTS", False);
    platform.utf8String = XInternAtom(d, "UTF8_STRING", False);
    platform.targets = XInternAtom(d, "TARGETS", False);
    platform.multiple = XInternAtom(d, "MULTIPLE", False);
    platform.incr = XInternAtom(d, "INCR", False);
    platform.clipboard = XInternAtom(d, "CLIPBOARD", False);
    platform.rayvkSelection = XInternAtom(d, "RAYVULKAN_SELECTION", False);
    platform.textUriList = XInternAtom(d, "text/uri-list", False);
    platform.imagePng = XInternAtom(d, "image/png", False);
    platform.atomText = XInternAtom(d, "TEXT", False);
    platform.atomTimestamp = XInternAtom(d, "TIMESTAMP", False);
    platform.xdndAware = XInternAtom(d, "XdndAware", False);
    platform.xdndEnter = XInternAtom(d, "XdndEnter", False);
    platform.xdndPosition = XInternAtom(d, "XdndPosition", False);
    platform.xdndStatus = XInternAtom(d, "XdndStatus", False);
    platform.xdndDrop = XInternAtom(d, "XdndDrop", False);
    platform.xdndFinished = XInternAtom(d, "XdndFinished", False);
    platform.xdndSelection = XInternAtom(d, "XdndSelection", False);
    platform.xdndActionCopy = XInternAtom(d, "XdndActionCopy", False);
    platform.xdndTypeList = XInternAtom(d, "XdndTypeList", False);

    return 0;
}

//----------------------------------------------------------------------------------
// Window state helpers
//----------------------------------------------------------------------------------

// Send an EWMH _NET_WM_STATE ClientMessage; action 0 = remove, 1 = add, 2 = toggle
static void SendStateMessage(Atom state1, Atom state2, int action)
{
    if (!WindowValid()) return;

    XEvent event = { 0 };
    event.type = ClientMessage;
    event.xclient.window = platform.handle;
    event.xclient.format = 32;
    event.xclient.message_type = platform.netWmState;
    event.xclient.data.l[0] = action;
    event.xclient.data.l[1] = (long)state1;
    event.xclient.data.l[2] = (long)state2;
    event.xclient.data.l[3] = 1;    // Normal application source indication

    XSendEvent(platform.display, platform.root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &event);
    XFlush(platform.display);
}

// Set or clear window manager decorations through _MOTIF_WM_HINTS
static void ApplyMotifDecorations(bool decorated)
{
    if (!WindowValid()) return;

    struct { unsigned long flags, functions, decorations; long inputMode; unsigned long status; } hints = { 0, 0, 0, 0, 0 };
    hints.flags = 2;    // MWM_HINTS_DECORATIONS
    hints.decorations = decorated? 1 : 0;

    XChangeProperty(platform.display, platform.handle, platform.motifWmHints, platform.motifWmHints,
                    32, PropModeReplace, (const unsigned char *)&hints, 5);
}

// Push CORE.window.screenMin/screenMax and the resizable flag into WM_NORMAL_HINTS
static void ApplySizeHints(void)
{
    if (!WindowValid()) return;

    XSizeHints *hints = XAllocSizeHints();
    if (hints == NULL) return;

    if ((CORE.window.flags & FLAG_WINDOW_RESIZABLE) == 0)
    {
        hints->flags |= (PMinSize | PMaxSize);
        hints->min_width = hints->max_width = CORE.window.screen.width;
        hints->min_height = hints->max_height = CORE.window.screen.height;
    }
    else
    {
        if ((CORE.window.screenMin.width > 0) && (CORE.window.screenMin.height > 0))
        {
            hints->flags |= PMinSize;
            hints->min_width = CORE.window.screenMin.width;
            hints->min_height = CORE.window.screenMin.height;
        }
        if ((CORE.window.screenMax.width > 0) && (CORE.window.screenMax.height > 0))
        {
            hints->flags |= PMaxSize;
            hints->max_width = CORE.window.screenMax.width;
            hints->max_height = CORE.window.screenMax.height;
        }
    }

    XSetWMNormalHints(platform.display, platform.handle, hints);
    XFree(hints);
}

// Apply a new window size coming from the server to CORE and the renderer
static void UpdateWindowSize(int width, int height)
{
    if ((width <= 0) || (height <= 0)) return;
    if ((width == CORE.window.screen.width) && (height == CORE.window.screen.height)) return;

    CORE.window.screen.width = width;
    CORE.window.screen.height = height;
    CORE.window.render.width = (int)((float)width*CORE.window.scale.x + 0.5f);
    CORE.window.render.height = (int)((float)height*CORE.window.scale.y + 0.5f);
    if (CORE.window.render.width <= 0) CORE.window.render.width = width;
    if (CORE.window.render.height <= 0) CORE.window.render.height = height;

    CORE.window.resizedLastFrame = true;
    if (rlvkIsReady()) rlvkResize(CORE.window.render.width, CORE.window.render.height);
}

// Warp the pointer to the window center, the reference point for locked-cursor deltas
static void CenterCursorForLock(void)
{
    platform.lockCenterX = CORE.window.screen.width/2;
    platform.lockCenterY = CORE.window.screen.height/2;
    XWarpPointer(platform.display, None, platform.handle, 0, 0, 0, 0,
                 platform.lockCenterX, platform.lockCenterY);
    XFlush(platform.display);
}

//----------------------------------------------------------------------------------
// Monitors (XRandR)
//----------------------------------------------------------------------------------

// Fill the caller array with the connected monitors; returns how many were written
static int GetMonitors(MonitorInfo *monitors, int maxCount)
{
    if ((platform.display == NULL) || (maxCount <= 0)) return 0;

    int count = 0;

    if (platform.randrAvailable)
    {
        XRRScreenResources *res = XRRGetScreenResourcesCurrent(platform.display, platform.root);
        if (res != NULL)
        {
            for (int i = 0; (i < res->noutput) && (count < maxCount); i++)
            {
                XRROutputInfo *output = XRRGetOutputInfo(platform.display, res, res->outputs[i]);
                if (output == NULL) continue;
                if ((output->connection != RR_Connected) || (output->crtc == 0)) { XRRFreeOutputInfo(output); continue; }

                XRRCrtcInfo *crtc = XRRGetCrtcInfo(platform.display, res, output->crtc);
                if (crtc == NULL) { XRRFreeOutputInfo(output); continue; }

                MonitorInfo *m = &monitors[count];
                memset(m, 0, sizeof(*m));
                m->x = crtc->x;
                m->y = crtc->y;
                m->width = (int)crtc->width;
                m->height = (int)crtc->height;
                m->physicalWidth = (int)output->mm_width;
                m->physicalHeight = (int)output->mm_height;

                for (int k = 0; k < res->nmode; k++)
                {
                    if (res->modes[k].id != crtc->mode) continue;
                    unsigned long total = (unsigned long)res->modes[k].hTotal*(unsigned long)res->modes[k].vTotal;
                    if (total > 0) m->refreshRate = (int)((double)res->modes[k].dotClock/(double)total + 0.5);
                    break;
                }

                if (output->name != NULL)
                {
                    strncpy(m->name, output->name, sizeof(m->name) - 1);
                    m->name[sizeof(m->name) - 1] = '\0';
                }

                count++;
                XRRFreeCrtcInfo(crtc);
                XRRFreeOutputInfo(output);
            }
            XRRFreeScreenResources(res);
        }
    }

    if (count == 0)
    {
        // Fallback: the X screen itself is the only monitor
        MonitorInfo *m = &monitors[0];
        memset(m, 0, sizeof(*m));
        m->width = DisplayWidth(platform.display, platform.screen);
        m->height = DisplayHeight(platform.display, platform.screen);
        m->physicalWidth = DisplayWidthMM(platform.display, platform.screen);
        m->physicalHeight = DisplayHeightMM(platform.display, platform.screen);
        m->refreshRate = 60;
        strcpy(m->name, "X11 Screen");
        count = 1;
    }

    return count;
}

// Content scale derived from the Xft.dpi X resource; 1.0 when unset or HighDPI is off
static float GetDisplayScale(void)
{
    if ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) == 0) return 1.0f;
    if (platform.display == NULL) return 1.0f;

    float scale = 1.0f;
    char *resources = XResourceManagerString(platform.display);
    if (resources != NULL)
    {
        XrmDatabase db = XrmGetStringDatabase(resources);
        if (db != NULL)
        {
            char *type = NULL;
            XrmValue value = { 0, NULL };
            if ((XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) == True) && (value.addr != NULL))
            {
                double dpi = atof(value.addr);
                if (dpi > 1.0) scale = (float)(dpi/96.0);
            }
            XrmDestroyDatabase(db);
        }
    }

    if (scale < 1.0f) scale = 1.0f;
    return scale;
}

//----------------------------------------------------------------------------------
// Selections (clipboard + Xdnd payloads)
//----------------------------------------------------------------------------------

// XIfEvent predicate matching a SelectionNotify addressed to our window
static Bool MatchSelectionNotify(Display *display, XEvent *event, XPointer arg)
{
    (void)display;
    Window window = *(Window *)arg;
    return (event->type == SelectionNotify) && (event->xselection.requestor == window);
}

// XIfEvent predicate matching an INCR PropertyNotify on our transfer property
static Bool MatchPropertyNotify(Display *display, XEvent *event, XPointer arg)
{
    (void)display;
    Window window = *(Window *)arg;
    return (event->type == PropertyNotify) && (event->xproperty.window == window) &&
           (event->xproperty.state == PropertyNewValue);
}

// Convert a selection and read the answer; returns an RL_MALLOC'd NUL-terminated buffer
// or NULL. *outSize receives the payload size without the terminator.
static unsigned char *ReadSelection(Atom selection, Atom target, unsigned long *outSize)
{
    if (outSize != NULL) *outSize = 0;
    if (!WindowValid()) return NULL;

    XDeleteProperty(platform.display, platform.handle, platform.rayvkSelection);
    XConvertSelection(platform.display, selection, target, platform.rayvkSelection,
                      platform.handle, CurrentTime);
    XFlush(platform.display);

    XEvent event = { 0 };
    Window self = platform.handle;
    double deadline = rlLinuxGetTime() + SELECTION_TIMEOUT;
    bool got = false;
    while (rlLinuxGetTime() < deadline)
    {
        if (XCheckIfEvent(platform.display, &event, MatchSelectionNotify, (XPointer)&self) == True) { got = true; break; }
        rlLinuxWaitTime(0.002);
    }

    if (!got) { TRACELOG(LOG_WARNING, "PLATFORM: Timed out waiting for selection owner"); return NULL; }
    if (event.xselection.property == None) return NULL;

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long items = 0, bytesLeft = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(platform.display, platform.handle, platform.rayvkSelection, 0, LONG_MAX_ITEMS,
                           False, AnyPropertyType, &actualType, &actualFormat, &items, &bytesLeft, &data) != Success)
    {
        return NULL;
    }

    unsigned char *result = NULL;
    unsigned long size = 0;

    if (actualType == platform.incr)
    {
        // Incremental transfer: the owner appends chunks until an empty property arrives
        if (data != NULL) XFree(data);
        XDeleteProperty(platform.display, platform.handle, platform.rayvkSelection);
        XFlush(platform.display);

        deadline = rlLinuxGetTime() + SELECTION_TIMEOUT*10.0;
        for (;;)
        {
            if (rlLinuxGetTime() >= deadline) { TRACELOG(LOG_WARNING, "PLATFORM: INCR selection transfer timed out"); break; }
            if (XCheckIfEvent(platform.display, &event, MatchPropertyNotify, (XPointer)&self) != True) { rlLinuxWaitTime(0.002); continue; }
            if (event.xproperty.atom != platform.rayvkSelection) continue;

            data = NULL;
            items = 0;
            if (XGetWindowProperty(platform.display, platform.handle, platform.rayvkSelection, 0, LONG_MAX_ITEMS,
                                   False, AnyPropertyType, &actualType, &actualFormat, &items, &bytesLeft, &data) != Success) break;

            unsigned long chunk = items*(unsigned long)(actualFormat/8);
            if (chunk == 0) { if (data != NULL) XFree(data); XDeleteProperty(platform.display, platform.handle, platform.rayvkSelection); break; }

            unsigned char *grown = (unsigned char *)RL_REALLOC(result, size + chunk + 1);
            if (grown == NULL) { if (data != NULL) XFree(data); break; }
            result = grown;
            memcpy(result + size, data, chunk);
            size += chunk;
            result[size] = '\0';

            XFree(data);
            XDeleteProperty(platform.display, platform.handle, platform.rayvkSelection);
            XFlush(platform.display);
            deadline = rlLinuxGetTime() + SELECTION_TIMEOUT*10.0;
        }
    }
    else if (data != NULL)
    {
        size = items*(unsigned long)(actualFormat/8);
        result = (unsigned char *)RL_MALLOC(size + 1);
        if (result != NULL)
        {
            memcpy(result, data, size);
            result[size] = '\0';
        }
        else size = 0;
        XFree(data);
        XDeleteProperty(platform.display, platform.handle, platform.rayvkSelection);
    }

    if (outSize != NULL) *outSize = size;
    return result;
}

// Answer an ICCCM SelectionRequest for the clipboard text we own
static void HandleSelectionRequest(const XSelectionRequestEvent *request)
{
    XEvent reply = { 0 };
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = request->display;
    reply.xselection.requestor = request->requestor;
    reply.xselection.selection = request->selection;
    reply.xselection.target = request->target;
    reply.xselection.time = request->time;
    reply.xselection.property = None;

    Atom property = (request->property != None)? request->property : request->target;
    const char *text = platform.clipboardOwnedText;

    if (request->target == platform.targets)
    {
        Atom supported[4] = { platform.targets, platform.utf8String, XA_STRING, platform.atomText };
        XChangeProperty(platform.display, request->requestor, property, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)supported, 4);
        reply.xselection.property = property;
    }
    else if ((text != NULL) &&
             ((request->target == platform.utf8String) || (request->target == XA_STRING) || (request->target == platform.atomText)))
    {
        XChangeProperty(platform.display, request->requestor, property, request->target, 8, PropModeReplace,
                        (const unsigned char *)text, (int)strlen(text));
        reply.xselection.property = property;
    }

    XSendEvent(platform.display, request->requestor, False, 0, &reply);
    XFlush(platform.display);
}

//----------------------------------------------------------------------------------
// Drag and drop (Xdnd v5)
//----------------------------------------------------------------------------------

// Release the currently stored dropped file paths
static void ClearDroppedFiles(void)
{
    if (CORE.window.dropFilepaths == NULL) return;

    for (unsigned int i = 0; i < CORE.window.dropFileCount; i++) RL_FREE(CORE.window.dropFilepaths[i]);
    RL_FREE(CORE.window.dropFilepaths);
    CORE.window.dropFilepaths = NULL;
    CORE.window.dropFileCount = 0;
}

// Percent-decode a file:// URI into a plain filesystem path
static void DecodeFileUri(const char *uri, int length, char *out, int outSize)
{
    int start = 0;
    if ((length >= 7) && (strncmp(uri, "file://", 7) == 0))
    {
        start = 7;
        // Skip an optional hostname component ("file://host/path")
        while ((start < length) && (uri[start] != '/')) start++;
    }

    int w = 0;
    for (int i = start; (i < length) && (w < (outSize - 1)); i++)
    {
        if ((uri[i] == '%') && ((i + 2) < length))
        {
            char hex[3] = { uri[i + 1], uri[i + 2], '\0' };
            char *end = NULL;
            long value = strtol(hex, &end, 16);
            if ((end != NULL) && (*end == '\0'))
            {
                out[w++] = (char)value;
                i += 2;
                continue;
            }
        }
        out[w++] = uri[i];
    }
    out[w] = '\0';
}

// Store a text/uri-list payload into CORE.window.dropFilepaths
static void StoreDroppedUriList(const char *uriList, unsigned long size)
{
    ClearDroppedFiles();

    CORE.window.dropFilepaths = (char **)RL_CALLOC(MAX_DROPPED_FILES, sizeof(char *));
    if (CORE.window.dropFilepaths == NULL) return;

    unsigned long i = 0;
    while ((i < size) && (CORE.window.dropFileCount < MAX_DROPPED_FILES))
    {
        while ((i < size) && ((uriList[i] == '\r') || (uriList[i] == '\n'))) i++;
        if (i >= size) break;

        unsigned long start = i;
        while ((i < size) && (uriList[i] != '\r') && (uriList[i] != '\n')) i++;

        int length = (int)(i - start);
        if ((length == 0) || (uriList[start] == '#')) continue;   // Comment lines per RFC 2483

        char *path = (char *)RL_CALLOC(MAX_FILEPATH_LENGTH, 1);
        if (path == NULL) break;
        DecodeFileUri(uriList + start, length, path, MAX_FILEPATH_LENGTH);
        if (path[0] == '\0') { RL_FREE(path); continue; }

        CORE.window.dropFilepaths[CORE.window.dropFileCount] = path;
        CORE.window.dropFileCount++;
    }

    if (CORE.window.dropFileCount == 0) ClearDroppedFiles();
    else TRACELOG(LOG_INFO, "PLATFORM: %u file(s) dropped on window", CORE.window.dropFileCount);
}

// Process the Xdnd protocol ClientMessages (Enter/Position/Drop)
static void HandleXdndClientMessage(const XClientMessageEvent *message)
{
    if (message->message_type == platform.xdndEnter)
    {
        platform.xdndSource = (Window)message->data.l[0];
        platform.xdndVersion = (int)((unsigned long)message->data.l[1] >> 24);
        platform.xdndFormat = None;

        bool useTypeList = ((message->data.l[1] & 1) != 0);
        if (!useTypeList)
        {
            for (int i = 2; i <= 4; i++)
            {
                if ((Atom)message->data.l[i] == platform.textUriList) platform.xdndFormat = platform.textUriList;
            }
            return;
        }

        Atom actualType = None;
        int actualFormat = 0;
        unsigned long items = 0, bytesLeft = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(platform.display, platform.xdndSource, platform.xdndTypeList, 0, MAX_XDND_TYPES,
                               False, XA_ATOM, &actualType, &actualFormat, &items, &bytesLeft, &data) == Success)
        {
            Atom *types = (Atom *)(void *)data;
            for (unsigned long i = 0; (types != NULL) && (i < items); i++)
            {
                if (types[i] == platform.textUriList) platform.xdndFormat = platform.textUriList;
            }
            if (data != NULL) XFree(data);
        }
    }
    else if (message->message_type == platform.xdndPosition)
    {
        if (platform.xdndVersion > 5) return;

        XEvent reply = { 0 };
        reply.type = ClientMessage;
        reply.xclient.window = (Window)message->data.l[0];
        reply.xclient.message_type = platform.xdndStatus;
        reply.xclient.format = 32;
        reply.xclient.data.l[0] = (long)platform.handle;
        reply.xclient.data.l[1] = (platform.xdndFormat != None)? 1 : 0;
        reply.xclient.data.l[2] = 0;
        reply.xclient.data.l[3] = 0;
        reply.xclient.data.l[4] = (long)platform.xdndActionCopy;

        XSendEvent(platform.display, (Window)message->data.l[0], False, NoEventMask, &reply);
        XFlush(platform.display);
    }
    else if (message->message_type == platform.xdndDrop)
    {
        platform.xdndTimestamp = (platform.xdndVersion >= 1)? (Time)message->data.l[2] : CurrentTime;

        if (platform.xdndFormat == None)
        {
            XEvent reply = { 0 };
            reply.type = ClientMessage;
            reply.xclient.window = platform.xdndSource;
            reply.xclient.message_type = platform.xdndFinished;
            reply.xclient.format = 32;
            reply.xclient.data.l[0] = (long)platform.handle;
            reply.xclient.data.l[1] = 0;
            reply.xclient.data.l[2] = (long)None;
            XSendEvent(platform.display, platform.xdndSource, False, NoEventMask, &reply);
            XFlush(platform.display);
            return;
        }

        XConvertSelection(platform.display, platform.xdndSelection, platform.xdndFormat,
                          platform.xdndSelection, platform.handle, platform.xdndTimestamp);
        XFlush(platform.display);
    }
}

// Consume the text/uri-list answer of an Xdnd drop and acknowledge the source
static void HandleXdndSelection(const XSelectionEvent *selection)
{
    if (selection->property == None) return;

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long items = 0, bytesLeft = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(platform.display, platform.handle, platform.xdndSelection, 0, LONG_MAX_ITEMS,
                           False, AnyPropertyType, &actualType, &actualFormat, &items, &bytesLeft, &data) == Success)
    {
        if ((data != NULL) && (items > 0)) StoreDroppedUriList((const char *)data, items*(unsigned long)(actualFormat/8));
        if (data != NULL) XFree(data);
    }

    XDeleteProperty(platform.display, platform.handle, platform.xdndSelection);

    if (platform.xdndSource != 0)
    {
        XEvent reply = { 0 };
        reply.type = ClientMessage;
        reply.xclient.window = platform.xdndSource;
        reply.xclient.message_type = platform.xdndFinished;
        reply.xclient.format = 32;
        reply.xclient.data.l[0] = (long)platform.handle;
        reply.xclient.data.l[1] = 1;
        reply.xclient.data.l[2] = (long)platform.xdndActionCopy;
        XSendEvent(platform.display, platform.xdndSource, False, NoEventMask, &reply);
        XFlush(platform.display);
    }

    platform.xdndSource = 0;
    platform.xdndFormat = None;
}

//----------------------------------------------------------------------------------
// Event processing
//----------------------------------------------------------------------------------

// Translate one X11 button number into a raylib MouseButton, -1 when it is not a button
static int TranslateMouseButton(unsigned int button)
{
    switch (button)
    {
        case Button1: return MOUSE_BUTTON_LEFT;
        case Button2: return MOUSE_BUTTON_MIDDLE;
        case Button3: return MOUSE_BUTTON_RIGHT;
        case 8: return MOUSE_BUTTON_SIDE;       // X11 "back" button
        case 9: return MOUSE_BUTTON_EXTRA;      // X11 "forward" button
        default: break;
    }
    return -1;
}

// Update CORE mouse position, honoring the locked-cursor delta accumulation mode
static void ProcessMotion(int x, int y)
{
    if (!CORE.input.mouse.cursorLocked)
    {
        CORE.input.mouse.currentPosition.x = (float)x;
        CORE.input.mouse.currentPosition.y = (float)y;
        return;
    }

    int dx = x - platform.lockCenterX;
    int dy = y - platform.lockCenterY;
    if ((dx == 0) && (dy == 0)) return;     // Event generated by our own warp

    CORE.input.mouse.currentPosition.x += (float)dx;
    CORE.input.mouse.currentPosition.y += (float)dy;
    XWarpPointer(platform.display, None, platform.handle, 0, 0, 0, 0,
                 platform.lockCenterX, platform.lockCenterY);
}

// Dispatch a single native event into the CORE state
static void ProcessEvent(XEvent *event)
{
    switch (event->type)
    {
        case ClientMessage:
        {
            if (event->xclient.message_type == platform.wmProtocols)
            {
                Atom protocol = (Atom)event->xclient.data.l[0];
                if (protocol == platform.wmDeleteWindow) CORE.window.shouldClose = true;
                else if (protocol == platform.wmTakeFocus)
                {
                    // ICCCM: the WM handed us the focus, claim it with the timestamp it supplied
                    XSetInputFocus(platform.display, platform.handle, RevertToParent, (Time)event->xclient.data.l[1]);
                }
            }
            else if ((event->xclient.message_type == platform.xdndEnter) ||
                     (event->xclient.message_type == platform.xdndPosition) ||
                     (event->xclient.message_type == platform.xdndDrop))
            {
                HandleXdndClientMessage(&event->xclient);
            }
        } break;
        case ConfigureNotify:
        {
            UpdateWindowSize(event->xconfigure.width, event->xconfigure.height);
            CORE.window.position.x = event->xconfigure.x;
            CORE.window.position.y = event->xconfigure.y;
        } break;
        case MapNotify:
        {
            platform.windowMapped = true;
            CORE.window.flags &= ~FLAG_WINDOW_MINIMIZED;
            CORE.window.flags &= ~FLAG_WINDOW_HIDDEN;
        } break;
        case UnmapNotify:
        {
            platform.windowMapped = false;
            CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
        } break;
        case FocusIn:
        {
            if ((event->xfocus.mode == NotifyGrab) || (event->xfocus.mode == NotifyUngrab)) break;
            CORE.window.flags &= ~FLAG_WINDOW_UNFOCUSED;
            if (platform.xic != NULL) XSetICFocus(platform.xic);
            if (CORE.input.mouse.cursorLocked) CenterCursorForLock();
        } break;
        case FocusOut:
        {
            if ((event->xfocus.mode == NotifyGrab) || (event->xfocus.mode == NotifyUngrab)) break;
            CORE.window.flags |= FLAG_WINDOW_UNFOCUSED;
            if (platform.xic != NULL) XUnsetICFocus(platform.xic);
            // Release every key so no key stays logically down while unfocused
            memset(CORE.input.keyboard.currentKeyState, 0, sizeof(CORE.input.keyboard.currentKeyState));
        } break;
        case EnterNotify: CORE.input.mouse.cursorOnScreen = true; break;
        case LeaveNotify: CORE.input.mouse.cursorOnScreen = false; break;
        case KeyPress:
        {
            KeySym sym = XLookupKeysym(&event->xkey, 0);
            int key = TranslateKeysym(sym);

            if ((key > 0) && (key < MAX_KEYBOARD_KEYS))
            {
                // Detectable auto-repeat is enabled: a press on an already-down key is a repeat
                if (CORE.input.keyboard.currentKeyState[key] == 1) CORE.input.keyboard.keyRepeatInFrame[key] = 1;

                CORE.input.keyboard.currentKeyState[key] = 1;
                if (CORE.input.keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
                {
                    CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount] = key;
                    CORE.input.keyboard.keyPressedQueueCount++;
                }
            }

            char buffer[64] = { 0 };
            int length = 0;
            if (platform.xic != NULL)
            {
                Status status = 0;
                length = Xutf8LookupString(platform.xic, &event->xkey, buffer, (int)sizeof(buffer) - 1, NULL, &status);
                if ((status != XLookupChars) && (status != XLookupBoth)) length = 0;
            }
            else
            {
                // Without an input method the string is latin-1, re-encode it as UTF-8
                char latin[32] = { 0 };
                int got = XLookupString(&event->xkey, latin, (int)sizeof(latin), NULL, NULL);
                for (int i = 0; (i < got) && (length < (int)sizeof(buffer) - 2); i++)
                {
                    unsigned char c = (unsigned char)latin[i];
                    if (c < 0x80) buffer[length++] = (char)c;
                    else
                    {
                        buffer[length++] = (char)(0xC0 | (c >> 6));
                        buffer[length++] = (char)(0x80 | (c & 0x3F));
                    }
                }
            }

            if (length > 0) PushCharsUtf8(buffer, length);
        } break;
        case KeyRelease:
        {
            KeySym sym = XLookupKeysym(&event->xkey, 0);
            int key = TranslateKeysym(sym);
            if ((key > 0) && (key < MAX_KEYBOARD_KEYS)) CORE.input.keyboard.currentKeyState[key] = 0;
        } break;
        case ButtonPress:
        case ButtonRelease:
        {
            unsigned int button = event->xbutton.button;
            bool pressed = (event->type == ButtonPress);

            if ((button >= Button4) && (button <= 7))
            {
                if (!pressed) break;    // Wheel "release" carries no information
                if (button == Button4) CORE.input.mouse.currentWheelMove.y += 1.0f;
                else if (button == Button5) CORE.input.mouse.currentWheelMove.y -= 1.0f;
                else if (button == 6) CORE.input.mouse.currentWheelMove.x -= 1.0f;
                else CORE.input.mouse.currentWheelMove.x += 1.0f;
                break;
            }

            int mouseButton = TranslateMouseButton(button);
            if ((mouseButton >= 0) && (mouseButton < MAX_MOUSE_BUTTONS)) CORE.input.mouse.currentButtonState[mouseButton] = pressed? 1 : 0;
            ProcessMotion(event->xbutton.x, event->xbutton.y);
        } break;
        case MotionNotify: ProcessMotion(event->xmotion.x, event->xmotion.y); break;
        case SelectionRequest: HandleSelectionRequest(&event->xselectionrequest); break;
        case SelectionClear:
        {
            RL_FREE(platform.clipboardOwnedText);
            platform.clipboardOwnedText = NULL;
        } break;
        case SelectionNotify:
        {
            if (event->xselection.selection == platform.xdndSelection) HandleXdndSelection(&event->xselection);
        } break;
        default: break;
    }
}

//----------------------------------------------------------------------------------
// Vulkan surface
//----------------------------------------------------------------------------------

// rlvk surface callback: create the VK_KHR_xlib_surface for our window
static uint64_t CreateSurfaceX11(void *instance, void *userData)
{
    (void)userData;

    PFN_vkCreateXlibSurfaceKHR createXlibSurface =
        (PFN_vkCreateXlibSurfaceKHR)vkGetInstanceProcAddr((VkInstance)instance, "vkCreateXlibSurfaceKHR");
    if (createXlibSurface == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: vkCreateXlibSurfaceKHR not available in the Vulkan instance");
        return 0;
    }

    VkXlibSurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .pNext = NULL,
        .flags = 0,
        .dpy = platform.display,
        .window = platform.handle
    };

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (createXlibSurface((VkInstance)instance, &info, NULL, &surface) != VK_SUCCESS)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create Vulkan Xlib surface");
        return 0;
    }

    return (uint64_t)surface;
}

//----------------------------------------------------------------------------------
// Platform contract: initialization
//----------------------------------------------------------------------------------

// Load the Xcursor/core cursor shapes plus the blank cursor used by HideCursor()
static void InitCursors(void)
{
    static const unsigned int shapes[MOUSE_CURSOR_NOT_ALLOWED + 1] = {
        XC_left_ptr, XC_left_ptr, XC_xterm, XC_crosshair, XC_hand2,
        XC_sb_h_double_arrow, XC_sb_v_double_arrow, XC_bottom_right_corner,
        XC_bottom_left_corner, XC_fleur, XC_X_cursor
    };
    static const char *themeNames[MOUSE_CURSOR_NOT_ALLOWED + 1] = {
        "default", "default", "text", "crosshair", "pointer",
        "ew-resize", "ns-resize", "nwse-resize", "nesw-resize", "all-scroll", "not-allowed"
    };

    for (int i = 0; i <= MOUSE_CURSOR_NOT_ALLOWED; i++)
    {
        platform.cursors[i] = XcursorLibraryLoadCursor(platform.display, themeNames[i]);
        if (platform.cursors[i] == None) platform.cursors[i] = XCreateFontCursor(platform.display, shapes[i]);
    }

    char blank[8] = { 0 };
    Pixmap pixmap = XCreateBitmapFromData(platform.display, platform.root, blank, 8, 8);
    XColor black = { 0 };
    platform.blankCursor = XCreatePixmapCursor(platform.display, pixmap, pixmap, &black, &black, 0, 0);
    XFreePixmap(platform.display, pixmap);
}

// Apply the flags that must be set right after the window is created
static void ApplyInitialFlags(void)
{
    if ((CORE.window.flags & FLAG_WINDOW_UNDECORATED) != 0) ApplyMotifDecorations(false);
    if ((CORE.window.flags & FLAG_WINDOW_TOPMOST) != 0) SendStateMessage(platform.netWmStateAbove, 0, 1);
    if ((CORE.window.flags & FLAG_FULLSCREEN_MODE) != 0)
    {
        SendStateMessage(platform.netWmStateFullscreen, 0, 1);
        CORE.window.fullscreen = true;
    }
    if ((CORE.window.flags & FLAG_WINDOW_MAXIMIZED) != 0)
    {
        SendStateMessage(platform.netWmStateMaximizedVert, platform.netWmStateMaximizedHorz, 1);
    }
    if ((CORE.window.flags & FLAG_WINDOW_MINIMIZED) != 0) XIconifyWindow(platform.display, platform.handle, platform.screen);
    if ((CORE.window.flags & FLAG_INTERLACED_HINT) != 0) TRACELOG(LOG_WARNING, "PLATFORM: FLAG_INTERLACED_HINT is not supported on X11");
}

int InitPlatform(void)
{
    XInitThreads();
    setlocale(LC_ALL, "");

    platform.display = XOpenDisplay(NULL);
    if (platform.display == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to open X display");
        return -1;
    }

    platform.screen = DefaultScreen(platform.display);
    platform.root = RootWindow(platform.display, platform.screen);
    InitAtoms();

    int eventBase = 0, errorBase = 0;
    platform.randrAvailable = (XRRQueryExtension(platform.display, &eventBase, &errorBase) == True);
    platform.xfixesAvailable = (XFixesQueryExtension(platform.display, &eventBase, &errorBase) == True);

    CORE.window.display.width = DisplayWidth(platform.display, platform.screen);
    CORE.window.display.height = DisplayHeight(platform.display, platform.screen);

    if (CORE.window.screen.width <= 0) CORE.window.screen.width = 800;
    if (CORE.window.screen.height <= 0) CORE.window.screen.height = 450;

    float scale = GetDisplayScale();
    CORE.window.scale.x = scale;
    CORE.window.scale.y = scale;

    // Pick a visual: a 32-bit ARGB one when a transparent framebuffer was requested
    platform.depth = DefaultDepth(platform.display, platform.screen);
    platform.visual = DefaultVisual(platform.display, platform.screen);
    bool ownColormap = false;

    if ((CORE.window.flags & FLAG_WINDOW_TRANSPARENT) != 0)
    {
        XVisualInfo info = { 0 };
        if (XMatchVisualInfo(platform.display, platform.screen, 32, TrueColor, &info) != 0)
        {
            platform.visual = info.visual;
            platform.depth = info.depth;
            ownColormap = true;
        }
        else TRACELOG(LOG_WARNING, "PLATFORM: No 32bit visual available, transparent framebuffer not supported");
    }

    platform.colormap = ownColormap?
        XCreateColormap(platform.display, platform.root, platform.visual, AllocNone) :
        DefaultColormap(platform.display, platform.screen);

    XSetWindowAttributes attributes = { 0 };
    attributes.colormap = platform.colormap;
    attributes.background_pixel = 0;
    attributes.border_pixel = 0;
    attributes.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | EnterWindowMask | LeaveWindowMask | StructureNotifyMask |
                            FocusChangeMask | ExposureMask | PropertyChangeMask | VisibilityChangeMask;

    platform.handle = XCreateWindow(platform.display, platform.root,
                                    0, 0, (unsigned int)CORE.window.screen.width, (unsigned int)CORE.window.screen.height,
                                    0, platform.depth, InputOutput, platform.visual,
                                    CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &attributes);
    if (platform.handle == 0)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create X11 window");
        XCloseDisplay(platform.display);
        platform.display = NULL;
        return -1;
    }

    Atom protocols[2] = { platform.wmDeleteWindow, platform.wmTakeFocus };
    XSetWMProtocols(platform.display, platform.handle, protocols, 2);

    // ICCCM: advertise that we want the keyboard focus, so a WM using the passive or
    // locally-active input model actually assigns it to us
    XWMHints *wmHints = XAllocWMHints();
    if (wmHints != NULL)
    {
        wmHints->flags = InputHint | StateHint;
        wmHints->input = True;
        wmHints->initial_state = NormalState;
        XSetWMHints(platform.display, platform.handle, wmHints);
        XFree(wmHints);
    }

    const char *title = (CORE.window.title != NULL)? CORE.window.title : "rayvulkan";
    XStoreName(platform.display, platform.handle, title);
    XChangeProperty(platform.display, platform.handle, platform.netWmName, platform.utf8String, 8,
                    PropModeReplace, (const unsigned char *)title, (int)strlen(title));

    XClassHint *classHint = XAllocClassHint();
    if (classHint != NULL)
    {
        classHint->res_name = (char *)title;
        classHint->res_class = (char *)"rayvulkan";
        XSetClassHint(platform.display, platform.handle, classHint);
        XFree(classHint);
    }

    long pid = (long)getpid();
    XChangeProperty(platform.display, platform.handle, platform.netWmPid, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)&pid, 1);

    long xdndVersion = 5;
    XChangeProperty(platform.display, platform.handle, platform.xdndAware, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)&xdndVersion, 1);

    ApplySizeHints();
    ApplyInitialFlags();

    // Input method for dead keys / compose sequences (optional, falls back gracefully)
    platform.xim = XOpenIM(platform.display, NULL, NULL, NULL);
    if (platform.xim != NULL)
    {
        platform.xic = XCreateIC(platform.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                 XNClientWindow, platform.handle, XNFocusWindow, platform.handle, (void *)NULL);
    }
    if (platform.xic == NULL) TRACELOG(LOG_WARNING, "PLATFORM: No X input method available, dead keys will not compose");

    Bool detectable = False;
    XkbSetDetectableAutoRepeat(platform.display, True, &detectable);

    InitCursors();

    if ((CORE.window.flags & FLAG_WINDOW_HIDDEN) == 0)
    {
        XMapWindow(platform.display, platform.handle);
        XFlush(platform.display);
    }

    // Read back the geometry the WM actually granted us
    XWindowAttributes granted = { 0 };
    if (XGetWindowAttributes(platform.display, platform.handle, &granted) != 0)
    {
        CORE.window.screen.width = granted.width;
        CORE.window.screen.height = granted.height;
        CORE.window.position.x = granted.x;
        CORE.window.position.y = granted.y;
    }

    CORE.window.render.width = (int)((float)CORE.window.screen.width*CORE.window.scale.x + 0.5f);
    CORE.window.render.height = (int)((float)CORE.window.screen.height*CORE.window.scale.y + 0.5f);
    CORE.window.previousScreen.width = CORE.window.screen.width;
    CORE.window.previousScreen.height = CORE.window.screen.height;
    CORE.window.previousPosition.x = CORE.window.position.x;
    CORE.window.previousPosition.y = CORE.window.position.y;

    static const char *extensions[2] = { "VK_KHR_surface", "VK_KHR_xlib_surface" };
    rlvkPlatformGlue glue = {
        .requiredExtensions = extensions,
        .requiredExtensionCount = 2,
        .createSurface = CreateSurfaceX11,
        .userData = NULL
    };

    if (!rlvkInit(&glue, CORE.window.render.width, CORE.window.render.height, (CORE.window.flags & FLAG_VSYNC_HINT) != 0))
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to initialize Vulkan rendering backend");
        ClosePlatform();
        return -1;
    }

    rlLinuxInitGamepads();

    CORE.window.ready = true;
    TRACELOG(LOG_INFO, "PLATFORM: Initialized successfully (X11)");
    return 0;
}

void ClosePlatform(void)
{
    rlLinuxCloseGamepads();

    if (platform.display == NULL) return;

    if (CORE.input.mouse.cursorLocked) EnableCursorPlatform();

    ClearDroppedFiles();
    RL_FREE(platform.clipboardOwnedText);
    platform.clipboardOwnedText = NULL;
    RL_FREE(platform.clipboardFetched);
    platform.clipboardFetched = NULL;

    if (platform.xic != NULL) { XDestroyIC(platform.xic); platform.xic = NULL; }
    if (platform.xim != NULL) { XCloseIM(platform.xim); platform.xim = NULL; }

    for (int i = 0; i <= MOUSE_CURSOR_NOT_ALLOWED; i++)
    {
        if (platform.cursors[i] != None) XFreeCursor(platform.display, platform.cursors[i]);
        platform.cursors[i] = None;
    }
    if (platform.blankCursor != None) { XFreeCursor(platform.display, platform.blankCursor); platform.blankCursor = None; }

    if (platform.handle != 0) { XDestroyWindow(platform.display, platform.handle); platform.handle = 0; }

    XCloseDisplay(platform.display);
    platform.display = NULL;
    CORE.window.ready = false;
}

//----------------------------------------------------------------------------------
// Platform contract: frame loop
//----------------------------------------------------------------------------------

void PollInputEventsPlatform(void)
{
    // Per-frame input bookkeeping (idempotent, see contract note at the top of the file)
    memcpy(CORE.input.keyboard.previousKeyState, CORE.input.keyboard.currentKeyState, sizeof(CORE.input.keyboard.currentKeyState));
    memset(CORE.input.keyboard.keyRepeatInFrame, 0, sizeof(CORE.input.keyboard.keyRepeatInFrame));
    CORE.input.keyboard.keyPressedQueueCount = 0;
    CORE.input.keyboard.charPressedQueueCount = 0;

    memcpy(CORE.input.mouse.previousButtonState, CORE.input.mouse.currentButtonState, sizeof(CORE.input.mouse.currentButtonState));
    CORE.input.mouse.previousWheelMove = CORE.input.mouse.currentWheelMove;
    CORE.input.mouse.currentWheelMove.x = 0.0f;
    CORE.input.mouse.currentWheelMove.y = 0.0f;
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;

    memcpy(CORE.input.touch.previousTouchState, CORE.input.touch.currentTouchState, sizeof(CORE.input.touch.currentTouchState));

    CORE.window.resizedLastFrame = false;

    rlLinuxPollGamepads();

    if (platform.display == NULL) return;

    XEvent event;
    if (CORE.window.eventWaiting && (XPending(platform.display) == 0))
    {
        XNextEvent(platform.display, &event);   // Blocks until something arrives
        if (XFilterEvent(&event, None) != True) ProcessEvent(&event);
    }

    while (XPending(platform.display) > 0)
    {
        XNextEvent(platform.display, &event);
        if (XFilterEvent(&event, None) == True) continue;    // Consumed by the input method
        ProcessEvent(&event);
    }

    XFlush(platform.display);
}

double GetTimePlatform(void) { return rlLinuxGetTime(); }
void WaitTimePlatform(double seconds) { rlLinuxWaitTime(seconds); }
void SwapScreenBufferPlatform(void) { rlvkEndFrame(); }

//----------------------------------------------------------------------------------
// Platform contract: window control
//----------------------------------------------------------------------------------

void SetWindowTitlePlatform(const char *title)
{
    if (!WindowValid() || (title == NULL)) return;

    XStoreName(platform.display, platform.handle, title);
    XChangeProperty(platform.display, platform.handle, platform.netWmName, platform.utf8String, 8,
                    PropModeReplace, (const unsigned char *)title, (int)strlen(title));
    XChangeProperty(platform.display, platform.handle, platform.netWmIconName, platform.utf8String, 8,
                    PropModeReplace, (const unsigned char *)title, (int)strlen(title));
    XFlush(platform.display);
}

void SetWindowPositionPlatform(int x, int y)
{
    if (!WindowValid()) return;
    XMoveWindow(platform.display, platform.handle, x, y);
    CORE.window.position.x = x;
    CORE.window.position.y = y;
    XFlush(platform.display);
}

void SetWindowSizePlatform(int width, int height)
{
    if (!WindowValid() || (width <= 0) || (height <= 0)) return;

    // Fixed-size windows need their hints relaxed before the WM honors the resize
    CORE.window.screen.width = width;
    CORE.window.screen.height = height;
    ApplySizeHints();
    XResizeWindow(platform.display, platform.handle, (unsigned int)width, (unsigned int)height);
    XFlush(platform.display);
}

void SetWindowMinSizePlatform(int width, int height)
{
    CORE.window.screenMin.width = MaxInt(0, width);
    CORE.window.screenMin.height = MaxInt(0, height);
    ApplySizeHints();
}

void SetWindowMaxSizePlatform(int width, int height)
{
    CORE.window.screenMax.width = MaxInt(0, width);
    CORE.window.screenMax.height = MaxInt(0, height);
    ApplySizeHints();
}

void SetWindowOpacityPlatform(float opacity)
{
    if (!WindowValid()) return;
    if (opacity < 0.0f) opacity = 0.0f;
    else if (opacity > 1.0f) opacity = 1.0f;

    unsigned long value = (unsigned long)(opacity*(double)0xFFFFFFFFu);
    XChangeProperty(platform.display, platform.handle, platform.netWmWindowOpacity, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)&value, 1);
    XFlush(platform.display);
}

void SetWindowIconPlatform(Image *images, int count)
{
    if (!WindowValid()) return;

    if ((images == NULL) || (count <= 0))
    {
        XDeleteProperty(platform.display, platform.handle, platform.netWmIcon);
        XFlush(platform.display);
        return;
    }

    // _NET_WM_ICON: sequence of (width, height, w*h ARGB pixels) in 32-bit "long" items
    unsigned long items = 0;
    for (int i = 0; i < count; i++)
    {
        if (images[i].format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) continue;
        if ((images[i].width <= 0) || (images[i].height <= 0) || (images[i].data == NULL)) continue;
        items += 2 + (unsigned long)images[i].width*(unsigned long)images[i].height;
    }

    if (items == 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Window icons require PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 images");
        return;
    }

    unsigned long *data = (unsigned long *)RL_CALLOC(items, sizeof(unsigned long));
    if (data == NULL) return;

    unsigned long offset = 0;
    for (int i = 0; i < count; i++)
    {
        if (images[i].format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) continue;
        if ((images[i].width <= 0) || (images[i].height <= 0) || (images[i].data == NULL)) continue;

        const unsigned char *pixels = (const unsigned char *)images[i].data;
        data[offset++] = (unsigned long)images[i].width;
        data[offset++] = (unsigned long)images[i].height;

        int total = images[i].width*images[i].height;
        for (int p = 0; p < total; p++)
        {
            data[offset++] = ((unsigned long)pixels[p*4 + 3] << 24) | ((unsigned long)pixels[p*4 + 0] << 16) |
                             ((unsigned long)pixels[p*4 + 1] << 8) | (unsigned long)pixels[p*4 + 2];
        }
    }

    XChangeProperty(platform.display, platform.handle, platform.netWmIcon, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)data, (int)offset);
    XFlush(platform.display);
    RL_FREE(data);
}

void SetWindowFocusedPlatform(void)
{
    if (!WindowValid()) return;

    XRaiseWindow(platform.display, platform.handle);

    XEvent event = { 0 };
    event.type = ClientMessage;
    event.xclient.window = platform.handle;
    event.xclient.format = 32;
    event.xclient.message_type = platform.netActiveWindow;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(platform.display, platform.root, False, SubstructureNotifyMask | SubstructureRedirectMask, &event);

    XSetInputFocus(platform.display, platform.handle, RevertToParent, CurrentTime);
    XFlush(platform.display);
}

void SetWindowMonitorPlatform(int monitor)
{
    MonitorInfo monitors[MAX_MONITORS] = { { 0 } };
    int count = GetMonitors(monitors, MAX_MONITORS);
    if ((monitor < 0) || (monitor >= count))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return;
    }

    bool wasFullscreen = CORE.window.fullscreen;
    if (wasFullscreen) SendStateMessage(platform.netWmStateFullscreen, 0, 0);

    SetWindowPositionPlatform(monitors[monitor].x + (monitors[monitor].width - CORE.window.screen.width)/2,
                              monitors[monitor].y + (monitors[monitor].height - CORE.window.screen.height)/2);

    if (wasFullscreen) SendStateMessage(platform.netWmStateFullscreen, 0, 1);
}

void SetWindowStatePlatform(unsigned int flags)
{
    if (!WindowValid()) return;

    if ((flags & FLAG_VSYNC_HINT) != 0) rlvkSetVSync(true);
    if ((flags & FLAG_FULLSCREEN_MODE) != 0) { SendStateMessage(platform.netWmStateFullscreen, 0, 1); CORE.window.fullscreen = true; }
    if ((flags & FLAG_WINDOW_RESIZABLE) != 0) ApplySizeHints();
    if ((flags & FLAG_WINDOW_UNDECORATED) != 0) ApplyMotifDecorations(false);
    if ((flags & FLAG_WINDOW_HIDDEN) != 0) XUnmapWindow(platform.display, platform.handle);
    if ((flags & FLAG_WINDOW_MINIMIZED) != 0) MinimizeWindowPlatform();
    if ((flags & FLAG_WINDOW_MAXIMIZED) != 0) MaximizeWindowPlatform();
    if ((flags & FLAG_WINDOW_UNFOCUSED) == 0) { /* focus is WM driven, nothing to do */ }
    if ((flags & FLAG_WINDOW_TOPMOST) != 0) SendStateMessage(platform.netWmStateAbove, 0, 1);
    if ((flags & FLAG_BORDERLESS_WINDOWED_MODE) != 0) ToggleBorderlessWindowedPlatform();

    if ((flags & FLAG_WINDOW_MOUSE_PASSTHROUGH) != 0)
    {
        if (platform.xfixesAvailable)
        {
            XserverRegion region = XFixesCreateRegion(platform.display, NULL, 0);
            XFixesSetWindowShapeRegion(platform.display, platform.handle, ShapeInput, 0, 0, region);
            XFixesDestroyRegion(platform.display, region);
        }
        else TRACELOG(LOG_WARNING, "PLATFORM: XFixes not available, mouse passthrough not supported");
    }

    if ((flags & (FLAG_MSAA_4X_HINT | FLAG_WINDOW_TRANSPARENT | FLAG_WINDOW_HIGHDPI)) != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: MSAA/transparent/HighDPI flags can only be set before InitWindow()");
    }

    XFlush(platform.display);
}

void ClearWindowStatePlatform(unsigned int flags)
{
    if (!WindowValid()) return;

    if ((flags & FLAG_VSYNC_HINT) != 0) rlvkSetVSync(false);
    if ((flags & FLAG_FULLSCREEN_MODE) != 0) { SendStateMessage(platform.netWmStateFullscreen, 0, 0); CORE.window.fullscreen = false; }
    if ((flags & FLAG_WINDOW_RESIZABLE) != 0) ApplySizeHints();
    if ((flags & FLAG_WINDOW_UNDECORATED) != 0) ApplyMotifDecorations(true);
    if ((flags & FLAG_WINDOW_HIDDEN) != 0) XMapWindow(platform.display, platform.handle);
    if ((flags & FLAG_WINDOW_MINIMIZED) != 0) RestoreWindowPlatform();
    if ((flags & FLAG_WINDOW_MAXIMIZED) != 0) SendStateMessage(platform.netWmStateMaximizedVert, platform.netWmStateMaximizedHorz, 0);
    if ((flags & FLAG_WINDOW_TOPMOST) != 0) SendStateMessage(platform.netWmStateAbove, 0, 0);

    if ((flags & FLAG_WINDOW_MOUSE_PASSTHROUGH) != 0)
    {
        if (platform.xfixesAvailable) XFixesSetWindowShapeRegion(platform.display, platform.handle, ShapeInput, 0, 0, None);
    }

    XFlush(platform.display);
}

void ToggleFullscreenPlatform(void)
{
    if (!WindowValid()) return;

    if (!CORE.window.fullscreen)
    {
        CORE.window.previousPosition.x = CORE.window.position.x;
        CORE.window.previousPosition.y = CORE.window.position.y;
        CORE.window.previousScreen.width = CORE.window.screen.width;
        CORE.window.previousScreen.height = CORE.window.screen.height;
        SendStateMessage(platform.netWmStateFullscreen, 0, 1);
        CORE.window.fullscreen = true;
        CORE.window.flags |= FLAG_FULLSCREEN_MODE;
    }
    else
    {
        SendStateMessage(platform.netWmStateFullscreen, 0, 0);
        CORE.window.fullscreen = false;
        CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;
    }
}

void ToggleBorderlessWindowedPlatform(void)
{
    if (!WindowValid()) return;

    if ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) == 0)
    {
        MonitorInfo monitors[MAX_MONITORS] = { { 0 } };
        int count = GetMonitors(monitors, MAX_MONITORS);
        int current = GetCurrentMonitorPlatform();
        if ((current < 0) || (current >= count)) current = 0;

        CORE.window.previousPosition.x = CORE.window.position.x;
        CORE.window.previousPosition.y = CORE.window.position.y;
        CORE.window.previousScreen.width = CORE.window.screen.width;
        CORE.window.previousScreen.height = CORE.window.screen.height;

        ApplyMotifDecorations(false);
        CORE.window.flags |= (FLAG_BORDERLESS_WINDOWED_MODE | FLAG_WINDOW_UNDECORATED);
        SendStateMessage(platform.netWmStateAbove, 0, 1);
        XMoveResizeWindow(platform.display, platform.handle, monitors[current].x, monitors[current].y,
                          (unsigned int)monitors[current].width, (unsigned int)monitors[current].height);
    }
    else
    {
        CORE.window.flags &= ~(FLAG_BORDERLESS_WINDOWED_MODE | FLAG_WINDOW_UNDECORATED);
        ApplyMotifDecorations(true);
        SendStateMessage(platform.netWmStateAbove, 0, 0);
        XMoveResizeWindow(platform.display, platform.handle, CORE.window.previousPosition.x, CORE.window.previousPosition.y,
                          (unsigned int)CORE.window.previousScreen.width, (unsigned int)CORE.window.previousScreen.height);
    }

    XFlush(platform.display);
}

void MaximizeWindowPlatform(void)
{
    SendStateMessage(platform.netWmStateMaximizedVert, platform.netWmStateMaximizedHorz, 1);
    CORE.window.flags |= FLAG_WINDOW_MAXIMIZED;
}

void MinimizeWindowPlatform(void)
{
    if (!WindowValid()) return;
    XIconifyWindow(platform.display, platform.handle, platform.screen);
    XFlush(platform.display);
    CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
}

void RestoreWindowPlatform(void)
{
    if (!WindowValid()) return;

    SendStateMessage(platform.netWmStateMaximizedVert, platform.netWmStateMaximizedHorz, 0);
    XMapWindow(platform.display, platform.handle);
    XRaiseWindow(platform.display, platform.handle);
    XFlush(platform.display);

    CORE.window.flags &= ~(FLAG_WINDOW_MAXIMIZED | FLAG_WINDOW_MINIMIZED);
}

void *GetWindowHandlePlatform(void) { return (void *)(uintptr_t)platform.handle; }

//----------------------------------------------------------------------------------
// Platform contract: monitors
//----------------------------------------------------------------------------------

int GetMonitorCountPlatform(void)
{
    MonitorInfo monitors[MAX_MONITORS] = { { 0 } };
    return GetMonitors(monitors, MAX_MONITORS);
}

int GetCurrentMonitorPlatform(void)
{
    MonitorInfo monitors[MAX_MONITORS] = { { 0 } };
    int count = GetMonitors(monitors, MAX_MONITORS);

    int cx = CORE.window.position.x + CORE.window.screen.width/2;
    int cy = CORE.window.position.y + CORE.window.screen.height/2;

    for (int i = 0; i < count; i++)
    {
        if ((cx >= monitors[i].x) && (cx < (monitors[i].x + monitors[i].width)) &&
            (cy >= monitors[i].y) && (cy < (monitors[i].y + monitors[i].height))) return i;
    }

    return 0;
}

// Query one monitor safely; returns false and zeroes out when the index is invalid
static bool GetMonitorChecked(int monitor, MonitorInfo *out)
{
    MonitorInfo monitors[MAX_MONITORS] = { { 0 } };
    int count = GetMonitors(monitors, MAX_MONITORS);

    memset(out, 0, sizeof(*out));
    if ((monitor < 0) || (monitor >= count))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return false;
    }

    *out = monitors[monitor];
    return true;
}

Vector2 GetMonitorPositionPlatform(int monitor)
{
    MonitorInfo info = { 0 };
    if (!GetMonitorChecked(monitor, &info)) return (Vector2){ 0.0f, 0.0f };
    return (Vector2){ (float)info.x, (float)info.y };
}

int GetMonitorWidthPlatform(int monitor)
{
    MonitorInfo info = { 0 };
    if (!GetMonitorChecked(monitor, &info)) return 0;
    return info.width;
}

int GetMonitorHeightPlatform(int monitor)
{
    MonitorInfo info = { 0 };
    if (!GetMonitorChecked(monitor, &info)) return 0;
    return info.height;
}

int GetMonitorPhysicalWidthPlatform(int monitor)
{
    MonitorInfo info = { 0 };
    if (!GetMonitorChecked(monitor, &info)) return 0;
    return info.physicalWidth;
}

int GetMonitorPhysicalHeightPlatform(int monitor)
{
    MonitorInfo info = { 0 };
    if (!GetMonitorChecked(monitor, &info)) return 0;
    return info.physicalHeight;
}

int GetMonitorRefreshRatePlatform(int monitor)
{
    MonitorInfo info = { 0 };
    if (!GetMonitorChecked(monitor, &info)) return 0;
    return info.refreshRate;
}

const char *GetMonitorNamePlatform(int monitor)
{
    static char name[64] = { 0 };

    MonitorInfo info = { 0 };
    if (!GetMonitorChecked(monitor, &info)) { name[0] = '\0'; return name; }

    strncpy(name, info.name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    return name;
}

Vector2 GetWindowScaleDPIPlatform(void) { return (Vector2){ CORE.window.scale.x, CORE.window.scale.y }; }

//----------------------------------------------------------------------------------
// Platform contract: cursor and clipboard
//----------------------------------------------------------------------------------

// Apply the cursor shape matching the current hidden/locked/shape state
static void RefreshCursor(void)
{
    if (!WindowValid()) return;

    if (CORE.input.mouse.cursorHidden || CORE.input.mouse.cursorLocked)
    {
        XDefineCursor(platform.display, platform.handle, platform.blankCursor);
    }
    else
    {
        int shape = CORE.input.mouse.cursor;
        if ((shape < 0) || (shape > MOUSE_CURSOR_NOT_ALLOWED)) shape = MOUSE_CURSOR_DEFAULT;
        XDefineCursor(platform.display, platform.handle, platform.cursors[shape]);
    }

    XFlush(platform.display);
}

void ShowCursorPlatform(void)
{
    CORE.input.mouse.cursorHidden = false;
    RefreshCursor();
}

void HideCursorPlatform(void)
{
    CORE.input.mouse.cursorHidden = true;
    RefreshCursor();
}

void EnableCursorPlatform(void)
{
    if (!WindowValid()) return;

    if (platform.cursorGrabbed)
    {
        XUngrabPointer(platform.display, CurrentTime);
        platform.cursorGrabbed = false;
    }

    CORE.input.mouse.cursorLocked = false;
    CORE.input.mouse.cursorHidden = false;
    RefreshCursor();
}

void DisableCursorPlatform(void)
{
    if (!WindowValid()) return;

    int result = XGrabPointer(platform.display, platform.handle, True,
                              ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                              GrabModeAsync, GrabModeAsync, platform.handle, None, CurrentTime);
    if (result != GrabSuccess) TRACELOG(LOG_WARNING, "PLATFORM: Failed to grab pointer for cursor lock");
    else platform.cursorGrabbed = true;

    CORE.input.mouse.cursorLocked = true;
    CORE.input.mouse.cursorHidden = true;
    RefreshCursor();
    CenterCursorForLock();
}

void SetMouseCursorPlatform(int cursor)
{
    if ((cursor < 0) || (cursor > MOUSE_CURSOR_NOT_ALLOWED))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Unsupported mouse cursor shape (%i)", cursor);
        return;
    }

    CORE.input.mouse.cursor = cursor;
    RefreshCursor();
}

void SetMousePositionPlatform(int x, int y)
{
    if (!WindowValid()) return;

    CORE.input.mouse.currentPosition.x = (float)x;
    CORE.input.mouse.currentPosition.y = (float)y;
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;

    XWarpPointer(platform.display, None, platform.handle, 0, 0, 0, 0, x, y);
    XFlush(platform.display);

    if (CORE.input.mouse.cursorLocked) { platform.lockCenterX = x; platform.lockCenterY = y; }
}

void SetClipboardTextPlatform(const char *text)
{
    if (!WindowValid() || (text == NULL)) return;

    RL_FREE(platform.clipboardOwnedText);
    size_t length = strlen(text);
    platform.clipboardOwnedText = (char *)RL_MALLOC(length + 1);
    if (platform.clipboardOwnedText == NULL) return;
    memcpy(platform.clipboardOwnedText, text, length + 1);

    XSetSelectionOwner(platform.display, platform.clipboard, platform.handle, CurrentTime);
    XSetSelectionOwner(platform.display, XA_PRIMARY, platform.handle, CurrentTime);
    XFlush(platform.display);
}

const char *GetClipboardTextPlatform(void)
{
    if (!WindowValid()) return NULL;

    // We own the selection: no round trip needed
    if (XGetSelectionOwner(platform.display, platform.clipboard) == platform.handle) return platform.clipboardOwnedText;

    unsigned long size = 0;
    unsigned char *data = ReadSelection(platform.clipboard, platform.utf8String, &size);
    if (data == NULL) data = ReadSelection(platform.clipboard, XA_STRING, &size);
    if (data == NULL) return NULL;

    RL_FREE(platform.clipboardFetched);
    platform.clipboardFetched = (char *)data;
    return platform.clipboardFetched;
}

Image GetClipboardImagePlatform(void)
{
    Image image = { 0 };
    if (!WindowValid()) return image;

    unsigned long size = 0;
    unsigned char *data = ReadSelection(platform.clipboard, platform.imagePng, &size);
    if ((data == NULL) || (size == 0))
    {
        RL_FREE(data);
        TRACELOG(LOG_WARNING, "PLATFORM: No image/png data available in the clipboard");
        return image;
    }

    image = LoadImageFromMemory(".png", data, (int)size);
    RL_FREE(data);
    return image;
}

//----------------------------------------------------------------------------------
// Platform contract: misc
//----------------------------------------------------------------------------------

void OpenURLPlatform(const char *url)
{
    if (url == NULL) return;

    // Refuse anything that could be turned into shell/argument injection
    if (strchr(url, '\'') != NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Provided URL could be malicious, not opened");
        return;
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        _exit(127);     // execlp only returns on failure
    }
    else if (pid < 0) TRACELOG(LOG_WARNING, "PLATFORM: Failed to fork for OpenURL()");
    else
    {
        // Reap without blocking the frame; the child exits almost immediately
        int status = 0;
        waitpid(pid, &status, WNOHANG);
    }
}

void SetGamepadVibrationPlatform(int gamepad, float leftMotor, float rightMotor, float duration)
{
    rlLinuxSetGamepadVibration(gamepad, leftMotor, rightMotor, duration);
}

int SetGamepadMappingsPlatform(const char *mappings) { return rlLinuxSetGamepadMappings(mappings); }
