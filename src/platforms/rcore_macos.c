/**********************************************************************************************
*
*   rcore_macos - raylib-on-vulkan platform backend for macOS (pure C, Objective-C runtime, no .m files)
*
*   Everything Cocoa is driven through the Objective-C runtime (objc_getClass,
*   sel_registerName, objc_msgSend) so the whole library stays plain C99. Display
*   enumeration uses the C-only CoreGraphics API, timing uses mach_absolute_time and the
*   Vulkan surface is a CAMetalLayer handed to VK_EXT_metal_surface (MoltenVK).
*
*   CONTRACT EXTENSION (documented as required by CONVENTIONS.md)
*     MoltenVK is a *portability* driver: the Vulkan instance must be created with
*     VK_KHR_portability_enumeration enabled AND the flag
*     VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR set in VkInstanceCreateInfo::flags,
*     otherwise vkEnumeratePhysicalDevices() returns zero devices. rlvkPlatformGlue had no
*     way to express this, so a single field was added to src/rlvk.h:
*
*         bool portability;   // set the portability enumeration instance flag
*
*     rlvk.c MUST honour it: when glue->portability is true, add
*     VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR to VkInstanceCreateInfo::flags and
*     enable the VK_KHR_portability_subset device extension if the physical device reports
*     it. This backend sets it to true; all other backends leave it false.
*
*   DESIGN CHOICES (deliberate, see report)
*     - Event handling is polling based: PollInputEventsPlatform() drains the queue with
*       nextEventMatchingMask:untilDate:inMode:dequeue: and interprets the NSEvents
*       directly. No NSApplication delegate and no NSWindow delegate is created; window
*       closing is detected via [window isVisible] and resizes by comparing the content
*       size every frame. This removes all runtime-subclass fragility from the hot path.
*     - Exactly one runtime subclass is created: an NSView subclass that implements the
*       NSDraggingDestination methods so drag & drop works. If the class cannot be built
*       the backend transparently falls back to a plain NSView (drag & drop disabled,
*       warning logged) - it is never fatal.
*     - Gamepads are NOT supported: the GameController framework is block-based
*       (Objective-C blocks cannot be produced from plain C without libBlocksRuntime and
*       compiler support). Gamepad functions log a warning and report no gamepads.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

// nanosleep() is hidden by <time.h> under a strict -std=c99 on Apple platforms
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
    #define _DARWIN_C_SOURCE
#endif

#include "rcore_internal.h"

#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>

#include <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include "volk.h"

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

//----------------------------------------------------------------------------------
// AppKit constants (copied from the SDK headers, they are ABI stable)
//----------------------------------------------------------------------------------
#define NS_STYLE_BORDERLESS             0
#define NS_STYLE_TITLED                 (1UL << 0)
#define NS_STYLE_CLOSABLE               (1UL << 1)
#define NS_STYLE_MINIATURIZABLE         (1UL << 2)
#define NS_STYLE_RESIZABLE              (1UL << 3)

#define NS_BACKING_BUFFERED             2
#define NS_ACTIVATION_POLICY_REGULAR    0
#define NS_COLLECTION_FULLSCREEN_PRIMARY (1UL << 7)

#define NS_EVENT_MASK_ANY               0xFFFFFFFFFFFFFFFFULL

#define NS_EVENT_LEFT_MOUSE_DOWN        1
#define NS_EVENT_LEFT_MOUSE_UP          2
#define NS_EVENT_RIGHT_MOUSE_DOWN       3
#define NS_EVENT_RIGHT_MOUSE_UP         4
#define NS_EVENT_MOUSE_MOVED            5
#define NS_EVENT_LEFT_MOUSE_DRAGGED     6
#define NS_EVENT_RIGHT_MOUSE_DRAGGED    7
#define NS_EVENT_MOUSE_ENTERED          8
#define NS_EVENT_MOUSE_EXITED           9
#define NS_EVENT_KEY_DOWN               10
#define NS_EVENT_KEY_UP                 11
#define NS_EVENT_FLAGS_CHANGED          12
#define NS_EVENT_SCROLL_WHEEL           22
#define NS_EVENT_OTHER_MOUSE_DOWN       25
#define NS_EVENT_OTHER_MOUSE_UP         26
#define NS_EVENT_OTHER_MOUSE_DRAGGED    27

// Device dependent modifier bits (used to tell left from right modifier keys)
#define NS_MOD_LEFT_SHIFT               0x00000002
#define NS_MOD_RIGHT_SHIFT              0x00000004
#define NS_MOD_LEFT_CONTROL             0x00000001
#define NS_MOD_RIGHT_CONTROL            0x00002000
#define NS_MOD_LEFT_OPTION              0x00000020
#define NS_MOD_RIGHT_OPTION             0x00000040
#define NS_MOD_LEFT_COMMAND             0x00000008
#define NS_MOD_RIGHT_COMMAND            0x00000010
#define NS_MOD_CAPS_LOCK                0x00010000

#define NS_DRAG_OPERATION_COPY          1

#define RAYVK_MAX_MONITORS              16

// Objective-C BOOL encoding differs between Intel (signed char) and Apple silicon (bool)
#if defined(__arm64__) || defined(__aarch64__)
    #define RAYVK_BOOL_ENCODING "B"
#else
    #define RAYVK_BOOL_ENCODING "c"
#endif

//----------------------------------------------------------------------------------
// objc_msgSend helpers - every call site must cast to the exact signature
//----------------------------------------------------------------------------------
#define SEL_(name)              sel_registerName(name)
#define CLS_(name)              ((id)objc_getClass(name))

#define MSG_ID(o, s)            (((id (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_ID_ID(o, s, a)      (((id (*)(id, SEL, id))objc_msgSend)((o), (s), (a)))
#define MSG_ID_PTR(o, s, a)     (((id (*)(id, SEL, const void *))objc_msgSend)((o), (s), (a)))
#define MSG_VOID(o, s)          (((void (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_VOID_ID(o, s, a)    (((void (*)(id, SEL, id))objc_msgSend)((o), (s), (a)))
#define MSG_VOID_BOOL(o, s, a)  (((void (*)(id, SEL, BOOL))objc_msgSend)((o), (s), (a)))
#define MSG_VOID_DBL(o, s, a)   (((void (*)(id, SEL, double))objc_msgSend)((o), (s), (a)))
#define MSG_VOID_UL(o, s, a)    (((void (*)(id, SEL, unsigned long))objc_msgSend)((o), (s), (a)))
#define MSG_VOID_PT(o, s, a)    (((void (*)(id, SEL, CGPoint))objc_msgSend)((o), (s), (a)))
#define MSG_VOID_SZ(o, s, a)    (((void (*)(id, SEL, CGSize))objc_msgSend)((o), (s), (a)))
#define MSG_BOOL(o, s)          (((BOOL (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_BOOL_ID(o, s, a)    (((BOOL (*)(id, SEL, id))objc_msgSend)((o), (s), (a)))
#define MSG_BOOL_SEL(o, s, a)   (((BOOL (*)(id, SEL, SEL))objc_msgSend)((o), (s), (a)))
#define MSG_BOOL_L(o, s, a)     (((BOOL (*)(id, SEL, long))objc_msgSend)((o), (s), (a)))
#define MSG_LONG(o, s)          (((long (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_ULONG(o, s)         (((unsigned long (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_ULLONG(o, s)        (((unsigned long long (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_USHORT(o, s)        (((unsigned short (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_UINT(o, s)          (((unsigned int (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_DBL(o, s)           (((double (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_CSTR(o, s)          (((const char *(*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_PTR(o, s)           (((void *(*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_PT(o, s)            (((CGPoint (*)(id, SEL))objc_msgSend)((o), (s)))
#define MSG_ID_IDX(o, s, i)     (((id (*)(id, SEL, unsigned long))objc_msgSend)((o), (s), (i)))

// CGRect (32 bytes) is returned through memory on x86_64 -> objc_msgSend_stret
static CGRect MsgRect(id object, SEL selector)
{
    CGRect result = CGRectMake(0.0, 0.0, 0.0, 0.0);
    if (object == nil) return result;

#if defined(__x86_64__)
    ((void (*)(CGRect *, id, SEL))objc_msgSend_stret)(&result, object, selector);
#else
    result = ((CGRect (*)(id, SEL))objc_msgSend)(object, selector);
#endif

    return result;
}

// Create an autoreleased NSString from UTF-8, nil on failure
static id NSStringFromUtf8(const char *text)
{
    if (text == NULL) return nil;
    return MSG_ID_PTR(CLS_("NSString"), SEL_("stringWithUTF8String:"), (const void *)text);
}

//----------------------------------------------------------------------------------
// Module state
//----------------------------------------------------------------------------------
typedef struct PlatformData {
    id app;                             // NSApplication
    id window;                          // NSWindow
    id view;                            // NSView (RayvulkanView when available)
    id layer;                           // CAMetalLayer
    id pool;                            // NSAutoreleasePool (lifetime of the platform)
    Class viewClass;                    // Runtime created NSView subclass (may be NULL)

    unsigned long styleMask;

    double timebase;                    // mach ticks -> seconds
    uint64_t timeBase;

    CGDirectDisplayID displays[RAYVK_MAX_MONITORS];
    int displayCount;

    int previousContentWidth;
    int previousContentHeight;

    char *clipboardText;                // Owned, returned by GetClipboardTextPlatform()
    char monitorName[128];              // Static return buffer (raylib semantics)
} PlatformData;

static PlatformData platform = { 0 };

//----------------------------------------------------------------------------------
// Module internal functions declaration
//----------------------------------------------------------------------------------
static uint64_t CreateSurfaceMetal(void *vkInstance, void *userData);

static int TranslateKey(unsigned short keyCode);
static void ProcessEvent(id event);
static void ProcessKeyEvent(id event, bool down);
static void ProcessFlagsChanged(id event);
static void ProcessMouseButton(id event, int button, bool down);
static void UpdateMousePositionFromEvent(id event);
static void RegisterCodepoints(id event);

static void RefreshDisplays(void);
static bool DisplayIsValid(int monitor);
static void UpdateWindowMetrics(void);
static void ApplyCursorShape(void);
static void ResetInputStates(void);
static Class BuildDraggingViewClass(void);
static void StoreDroppedPaths(id filenames);

//----------------------------------------------------------------------------------
// Keyboard translation (Carbon virtual key codes -> raylib keys)
//----------------------------------------------------------------------------------
static int TranslateKey(unsigned short keyCode)
{
    switch (keyCode)
    {
        case 0x00: return KEY_A;
        case 0x01: return KEY_S;
        case 0x02: return KEY_D;
        case 0x03: return KEY_F;
        case 0x04: return KEY_H;
        case 0x05: return KEY_G;
        case 0x06: return KEY_Z;
        case 0x07: return KEY_X;
        case 0x08: return KEY_C;
        case 0x09: return KEY_V;
        case 0x0B: return KEY_B;
        case 0x0C: return KEY_Q;
        case 0x0D: return KEY_W;
        case 0x0E: return KEY_E;
        case 0x0F: return KEY_R;
        case 0x10: return KEY_Y;
        case 0x11: return KEY_T;
        case 0x12: return KEY_ONE;
        case 0x13: return KEY_TWO;
        case 0x14: return KEY_THREE;
        case 0x15: return KEY_FOUR;
        case 0x16: return KEY_SIX;
        case 0x17: return KEY_FIVE;
        case 0x18: return KEY_EQUAL;
        case 0x19: return KEY_NINE;
        case 0x1A: return KEY_SEVEN;
        case 0x1B: return KEY_MINUS;
        case 0x1C: return KEY_EIGHT;
        case 0x1D: return KEY_ZERO;
        case 0x1E: return KEY_RIGHT_BRACKET;
        case 0x1F: return KEY_O;
        case 0x20: return KEY_U;
        case 0x21: return KEY_LEFT_BRACKET;
        case 0x22: return KEY_I;
        case 0x23: return KEY_P;
        case 0x24: return KEY_ENTER;
        case 0x25: return KEY_L;
        case 0x26: return KEY_J;
        case 0x27: return KEY_APOSTROPHE;
        case 0x28: return KEY_K;
        case 0x29: return KEY_SEMICOLON;
        case 0x2A: return KEY_BACKSLASH;
        case 0x2B: return KEY_COMMA;
        case 0x2C: return KEY_SLASH;
        case 0x2D: return KEY_N;
        case 0x2E: return KEY_M;
        case 0x2F: return KEY_PERIOD;
        case 0x30: return KEY_TAB;
        case 0x31: return KEY_SPACE;
        case 0x32: return KEY_GRAVE;
        case 0x33: return KEY_BACKSPACE;
        case 0x35: return KEY_ESCAPE;
        case 0x36: return KEY_RIGHT_SUPER;
        case 0x37: return KEY_LEFT_SUPER;
        case 0x38: return KEY_LEFT_SHIFT;
        case 0x39: return KEY_CAPS_LOCK;
        case 0x3A: return KEY_LEFT_ALT;
        case 0x3B: return KEY_LEFT_CONTROL;
        case 0x3C: return KEY_RIGHT_SHIFT;
        case 0x3D: return KEY_RIGHT_ALT;
        case 0x3E: return KEY_RIGHT_CONTROL;
        case 0x41: return KEY_KP_DECIMAL;
        case 0x43: return KEY_KP_MULTIPLY;
        case 0x45: return KEY_KP_ADD;
        case 0x47: return KEY_NUM_LOCK;         // Keypad clear
        case 0x4B: return KEY_KP_DIVIDE;
        case 0x4C: return KEY_KP_ENTER;
        case 0x4E: return KEY_KP_SUBTRACT;
        case 0x51: return KEY_KP_EQUAL;
        case 0x52: return KEY_KP_0;
        case 0x53: return KEY_KP_1;
        case 0x54: return KEY_KP_2;
        case 0x55: return KEY_KP_3;
        case 0x56: return KEY_KP_4;
        case 0x57: return KEY_KP_5;
        case 0x58: return KEY_KP_6;
        case 0x59: return KEY_KP_7;
        case 0x5B: return KEY_KP_8;
        case 0x5C: return KEY_KP_9;
        case 0x60: return KEY_F5;
        case 0x61: return KEY_F6;
        case 0x62: return KEY_F7;
        case 0x63: return KEY_F3;
        case 0x64: return KEY_F8;
        case 0x65: return KEY_F9;
        case 0x67: return KEY_F11;
        case 0x69: return KEY_PRINT_SCREEN;     // F13
        case 0x6B: return KEY_SCROLL_LOCK;      // F14
        case 0x6D: return KEY_F10;
        case 0x6E: return KEY_KB_MENU;
        case 0x6F: return KEY_F12;
        case 0x71: return KEY_PAUSE;            // F15
        case 0x72: return KEY_INSERT;           // Help
        case 0x73: return KEY_HOME;
        case 0x74: return KEY_PAGE_UP;
        case 0x75: return KEY_DELETE;           // Forward delete
        case 0x76: return KEY_F4;
        case 0x77: return KEY_END;
        case 0x78: return KEY_F2;
        case 0x79: return KEY_PAGE_DOWN;
        case 0x7A: return KEY_F1;
        case 0x7B: return KEY_LEFT;
        case 0x7C: return KEY_RIGHT;
        case 0x7D: return KEY_DOWN;
        case 0x7E: return KEY_UP;
        default: break;
    }

    return KEY_NULL;
}

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
// Displays (CoreGraphics, pure C)
//----------------------------------------------------------------------------------
static void RefreshDisplays(void)
{
    uint32_t count = 0;
    platform.displayCount = 0;

    if (CGGetActiveDisplayList(RAYVK_MAX_MONITORS, platform.displays, &count) != kCGErrorSuccess)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to enumerate displays");
        return;
    }

    platform.displayCount = (int)count;
}

static bool DisplayIsValid(int monitor)
{
    return ((monitor >= 0) && (monitor < platform.displayCount));
}

//----------------------------------------------------------------------------------
// Window metrics
//----------------------------------------------------------------------------------

// Recompute screen/render/scale/position from the live NSWindow, resizing the swapchain
static void UpdateWindowMetrics(void)
{
    if (platform.window == nil) return;

    CGRect content = MsgRect(platform.view, SEL_("frame"));
    int width = (int)content.size.width;
    int height = (int)content.size.height;
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    float scale = 1.0f;
    if ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0)
    {
        double backing = MSG_DBL(platform.window, SEL_("backingScaleFactor"));
        if (backing > 0.0) scale = (float)backing;
    }

    CORE.window.screen.width = width;
    CORE.window.screen.height = height;
    CORE.window.scale.x = scale;
    CORE.window.scale.y = scale;
    CORE.window.render.width = (int)(width*scale);
    CORE.window.render.height = (int)(height*scale);

    if (platform.layer != nil)
    {
        MSG_VOID_DBL(platform.layer, SEL_("setContentsScale:"), (double)scale);

        CGSize drawable;
        drawable.width = (double)CORE.window.render.width;
        drawable.height = (double)CORE.window.render.height;
        if (MSG_BOOL_SEL(platform.layer, SEL_("respondsToSelector:"), SEL_("setDrawableSize:")))
        {
            MSG_VOID_SZ(platform.layer, SEL_("setDrawableSize:"), drawable);
        }
    }

    // Window position, converted from the bottom-left Cocoa origin to raylib's top-left origin
    CGRect frame = MsgRect(platform.window, SEL_("frame"));
    CGRect primary = CGDisplayBounds(CGMainDisplayID());
    CORE.window.position.x = (int)frame.origin.x;
    CORE.window.position.y = (int)(primary.size.height - (frame.origin.y + frame.size.height));
}

static void ApplyCursorShape(void)
{
    if (CORE.input.mouse.cursorHidden || CORE.input.mouse.cursorLocked) return;

    const char *selectorName = "arrowCursor";
    switch (CORE.input.mouse.cursor)
    {
        case MOUSE_CURSOR_IBEAM: selectorName = "IBeamCursor"; break;
        case MOUSE_CURSOR_CROSSHAIR: selectorName = "crosshairCursor"; break;
        case MOUSE_CURSOR_POINTING_HAND: selectorName = "pointingHandCursor"; break;
        case MOUSE_CURSOR_RESIZE_EW: selectorName = "resizeLeftRightCursor"; break;
        case MOUSE_CURSOR_RESIZE_NS: selectorName = "resizeUpDownCursor"; break;
        case MOUSE_CURSOR_RESIZE_NWSE: selectorName = "_windowResizeNorthWestSouthEastCursor"; break;
        case MOUSE_CURSOR_RESIZE_NESW: selectorName = "_windowResizeNorthEastSouthWestCursor"; break;
        case MOUSE_CURSOR_RESIZE_ALL: selectorName = "closedHandCursor"; break;
        case MOUSE_CURSOR_NOT_ALLOWED: selectorName = "operationNotAllowedCursor"; break;
        default: selectorName = "arrowCursor"; break;
    }

    id cursorClass = CLS_("NSCursor");
    SEL selector = SEL_(selectorName);

    // The diagonal resize cursors are private API, fall back to the arrow if missing
    if (!MSG_BOOL_SEL(cursorClass, SEL_("respondsToSelector:"), selector)) selector = SEL_("arrowCursor");

    id cursor = MSG_ID(cursorClass, selector);
    if (cursor != nil) MSG_VOID(cursor, SEL_("set"));
}

//----------------------------------------------------------------------------------
// Drag & drop: runtime built NSView subclass implementing NSDraggingDestination
//----------------------------------------------------------------------------------

// Copy an NSArray of NSString paths into CORE.window.dropFilepaths
static void StoreDroppedPaths(id filenames)
{
    if (CORE.window.dropFilepaths != NULL)
    {
        for (unsigned int i = 0; i < CORE.window.dropFileCount; i++) RL_FREE(CORE.window.dropFilepaths[i]);
        RL_FREE(CORE.window.dropFilepaths);
        CORE.window.dropFilepaths = NULL;
        CORE.window.dropFileCount = 0;
    }

    if (filenames == nil) return;

    unsigned long count = MSG_ULONG(filenames, SEL_("count"));
    if (count == 0) return;
    if (count > MAX_DROPPED_FILES) count = MAX_DROPPED_FILES;

    CORE.window.dropFilepaths = (char **)RL_CALLOC((size_t)count, sizeof(char *));
    if (CORE.window.dropFilepaths == NULL) return;

    unsigned int stored = 0;
    for (unsigned long i = 0; i < count; i++)
    {
        id path = MSG_ID_IDX(filenames, SEL_("objectAtIndex:"), i);
        if (path == nil) continue;

        const char *utf8 = MSG_CSTR(path, SEL_("UTF8String"));
        if (utf8 == NULL) continue;

        char *copy = (char *)RL_CALLOC(MAX_FILEPATH_LENGTH, 1);
        if (copy == NULL) continue;

        strncpy(copy, utf8, MAX_FILEPATH_LENGTH - 1);
        CORE.window.dropFilepaths[stored] = copy;
        stored++;
    }

    CORE.window.dropFileCount = stored;
    TRACELOG(LOG_INFO, "PLATFORM: Dropped %u file(s) on window", CORE.window.dropFileCount);
}

static unsigned long ViewDraggingEntered(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    return NS_DRAG_OPERATION_COPY;
}

static BOOL ViewPrepareForDragOperation(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    return YES;
}

static BOOL ViewPerformDragOperation(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd;

    if (sender == nil) return NO;

    id pasteboard = MSG_ID(sender, SEL_("draggingPasteboard"));
    if (pasteboard == nil) return NO;

    id type = NSStringFromUtf8("NSFilenamesPboardType");
    id filenames = MSG_ID_ID(pasteboard, SEL_("propertyListForType:"), type);
    if (filenames == nil) return NO;

    StoreDroppedPaths(filenames);

    return YES;
}

// Create the NSView subclass handling file drops, NULL if the runtime refuses
static Class BuildDraggingViewClass(void)
{
    Class base = objc_getClass("NSView");
    if (base == NULL) return NULL;

    Class created = objc_allocateClassPair(base, "RayvulkanDropView", 0);
    if (created == NULL) return NULL;       // Name already taken (double init)

    bool ok = true;
    ok = ok && class_addMethod(created, SEL_("draggingEntered:"), (IMP)ViewDraggingEntered, "L@:@");
    ok = ok && class_addMethod(created, SEL_("prepareForDragOperation:"), (IMP)ViewPrepareForDragOperation,
                               RAYVK_BOOL_ENCODING "@:@");
    ok = ok && class_addMethod(created, SEL_("performDragOperation:"), (IMP)ViewPerformDragOperation,
                               RAYVK_BOOL_ENCODING "@:@");

    if (!ok)
    {
        objc_disposeClassPair(created);
        return NULL;
    }

    objc_registerClassPair(created);

    return created;
}

//----------------------------------------------------------------------------------
// Event processing
//----------------------------------------------------------------------------------

// Update CORE mouse position (raylib logical, top-left origin) from an NSEvent
static void UpdateMousePositionFromEvent(id event)
{
    if (CORE.input.mouse.cursorLocked)
    {
        double deltaX = MSG_DBL(event, SEL_("deltaX"));
        double deltaY = MSG_DBL(event, SEL_("deltaY"));
        CORE.input.mouse.currentPosition.x += (float)deltaX;
        CORE.input.mouse.currentPosition.y += (float)deltaY;
        return;
    }

    CGPoint point = MSG_PT(event, SEL_("locationInWindow"));
    CGRect content = MsgRect(platform.view, SEL_("frame"));

    CORE.input.mouse.currentPosition.x = (float)point.x;
    CORE.input.mouse.currentPosition.y = (float)(content.size.height - point.y);

    CORE.input.mouse.cursorOnScreen = ((point.x >= 0.0) && (point.x <= content.size.width) &&
                                       (point.y >= 0.0) && (point.y <= content.size.height));
}

static void ProcessMouseButton(id event, int button, bool down)
{
    if ((button < 0) || (button >= MAX_MOUSE_BUTTONS)) return;

    UpdateMousePositionFromEvent(event);

    CORE.input.mouse.currentButtonState[button] = down? 1 : 0;

    if (button < MAX_TOUCH_POINTS)
    {
        CORE.input.touch.currentTouchState[button] = down? 1 : 0;
        CORE.input.touch.position[button] = CORE.input.mouse.currentPosition;
        CORE.input.touch.pointId[button] = button;
    }

    int count = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) if (CORE.input.touch.currentTouchState[i] == 1) count++;
    CORE.input.touch.pointCount = count;
}

// Push the printable codepoints of a key event into the char queue
static void RegisterCodepoints(id event)
{
    id characters = MSG_ID(event, SEL_("characters"));
    if (characters == nil) return;

    const char *utf8 = MSG_CSTR(characters, SEL_("UTF8String"));
    if (utf8 == NULL) return;

    int index = 0;
    while (utf8[index] != '\0')
    {
        int size = 0;
        int codepoint = GetCodepointNext(utf8 + index, &size);
        if (size <= 0) break;
        index += size;

        if ((codepoint < 32) || (codepoint == 127)) continue;               // Control characters
        if ((codepoint >= 0xF700) && (codepoint <= 0xF8FF)) continue;       // Cocoa function key range

        if (CORE.input.keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE)
        {
            CORE.input.keyboard.charPressedQueue[CORE.input.keyboard.charPressedQueueCount] = codepoint;
            CORE.input.keyboard.charPressedQueueCount++;
        }
    }
}

static void ProcessKeyEvent(id event, bool down)
{
    unsigned short keyCode = MSG_USHORT(event, SEL_("keyCode"));
    int key = TranslateKey(keyCode);

    if ((key > 0) && (key < MAX_KEYBOARD_KEYS))
    {
        if (down)
        {
            bool repeat = (MSG_BOOL(event, SEL_("isARepeat")) != NO);
            if (repeat) CORE.input.keyboard.keyRepeatInFrame[key] = 1;

            CORE.input.keyboard.currentKeyState[key] = 1;

            if (CORE.input.keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
            {
                CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount] = key;
                CORE.input.keyboard.keyPressedQueueCount++;
            }
        }
        else CORE.input.keyboard.currentKeyState[key] = 0;
    }

    if (down) RegisterCodepoints(event);
}

// Modifier keys only produce flagsChanged events: derive press/release from the flag bits
static void ProcessFlagsChanged(id event)
{
    unsigned short keyCode = MSG_USHORT(event, SEL_("keyCode"));
    unsigned long long flags = MSG_ULLONG(event, SEL_("modifierFlags"));

    int key = TranslateKey(keyCode);
    if ((key <= 0) || (key >= MAX_KEYBOARD_KEYS)) return;

    unsigned long long mask = 0;
    switch (keyCode)
    {
        case 0x38: mask = NS_MOD_LEFT_SHIFT; break;
        case 0x3C: mask = NS_MOD_RIGHT_SHIFT; break;
        case 0x3B: mask = NS_MOD_LEFT_CONTROL; break;
        case 0x3E: mask = NS_MOD_RIGHT_CONTROL; break;
        case 0x3A: mask = NS_MOD_LEFT_OPTION; break;
        case 0x3D: mask = NS_MOD_RIGHT_OPTION; break;
        case 0x37: mask = NS_MOD_LEFT_COMMAND; break;
        case 0x36: mask = NS_MOD_RIGHT_COMMAND; break;
        case 0x39: mask = NS_MOD_CAPS_LOCK; break;
        default: return;
    }

    bool down = ((flags & mask) != 0);
    CORE.input.keyboard.currentKeyState[key] = down? 1 : 0;

    if (down && (CORE.input.keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE))
    {
        CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount] = key;
        CORE.input.keyboard.keyPressedQueueCount++;
    }
}

// Interpret one NSEvent; returns without forwarding key events so AppKit does not beep
static void ProcessEvent(id event)
{
    long type = MSG_LONG(event, SEL_("type"));
    bool forward = true;

    switch (type)
    {
        case NS_EVENT_KEY_DOWN: ProcessKeyEvent(event, true); forward = false; break;
        case NS_EVENT_KEY_UP: ProcessKeyEvent(event, false); forward = false; break;
        case NS_EVENT_FLAGS_CHANGED: ProcessFlagsChanged(event); break;

        case NS_EVENT_LEFT_MOUSE_DOWN: ProcessMouseButton(event, MOUSE_BUTTON_LEFT, true); break;
        case NS_EVENT_LEFT_MOUSE_UP: ProcessMouseButton(event, MOUSE_BUTTON_LEFT, false); break;
        case NS_EVENT_RIGHT_MOUSE_DOWN: ProcessMouseButton(event, MOUSE_BUTTON_RIGHT, true); break;
        case NS_EVENT_RIGHT_MOUSE_UP: ProcessMouseButton(event, MOUSE_BUTTON_RIGHT, false); break;
        case NS_EVENT_OTHER_MOUSE_DOWN:
        case NS_EVENT_OTHER_MOUSE_UP:
        {
            long number = MSG_LONG(event, SEL_("buttonNumber"));
            int button = MOUSE_BUTTON_MIDDLE;
            if (number == 3) button = MOUSE_BUTTON_SIDE;
            else if (number == 4) button = MOUSE_BUTTON_EXTRA;
            else if (number > 4) button = MOUSE_BUTTON_FORWARD;
            ProcessMouseButton(event, button, (type == NS_EVENT_OTHER_MOUSE_DOWN));
            break;
        }

        case NS_EVENT_MOUSE_MOVED:
        case NS_EVENT_LEFT_MOUSE_DRAGGED:
        case NS_EVENT_RIGHT_MOUSE_DRAGGED:
        case NS_EVENT_OTHER_MOUSE_DRAGGED: UpdateMousePositionFromEvent(event); break;

        case NS_EVENT_MOUSE_ENTERED: CORE.input.mouse.cursorOnScreen = true; break;
        case NS_EVENT_MOUSE_EXITED: CORE.input.mouse.cursorOnScreen = false; break;

        case NS_EVENT_SCROLL_WHEEL:
        {
            double deltaX = MSG_DBL(event, SEL_("scrollingDeltaX"));
            double deltaY = MSG_DBL(event, SEL_("scrollingDeltaY"));

            if (MSG_BOOL(event, SEL_("hasPreciseScrollingDeltas")) != NO)
            {
                deltaX *= 0.1;
                deltaY *= 0.1;
            }

            CORE.input.mouse.currentWheelMove.x += (float)deltaX;
            CORE.input.mouse.currentWheelMove.y += (float)deltaY;
            break;
        }
        default: break;
    }

    if (forward && (platform.app != nil)) MSG_VOID_ID(platform.app, SEL_("sendEvent:"), event);
}

//----------------------------------------------------------------------------------
// Vulkan surface glue
//----------------------------------------------------------------------------------
static uint64_t CreateSurfaceMetal(void *vkInstance, void *userData)
{
    (void)userData;

    VkInstance instance = (VkInstance)vkInstance;
    if ((instance == VK_NULL_HANDLE) || (platform.layer == nil)) return 0;

    PFN_vkCreateMetalSurfaceEXT createSurface =
        (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT");
    if (createSurface == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: vkCreateMetalSurfaceEXT not available (MoltenVK missing?)");
        return 0;
    }

    VkMetalSurfaceCreateInfoEXT createInfo = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pNext = NULL,
        .flags = 0,
        .pLayer = (const CAMetalLayer *)platform.layer
    };

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (createSurface(instance, &createInfo, NULL, &surface) != VK_SUCCESS)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create Vulkan Metal surface");
        return 0;
    }

#if defined(VK_USE_64_BIT_PTR_DEFINES) && (VK_USE_64_BIT_PTR_DEFINES == 1)
    return (uint64_t)(uintptr_t)surface;
#else
    return (uint64_t)surface;
#endif
}

//----------------------------------------------------------------------------------
// Platform contract: initialization
//----------------------------------------------------------------------------------
int InitPlatform(void)
{
    memset(&platform, 0, sizeof(platform));

    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase) != KERN_SUCCESS)
    {
        timebase.numer = 1;
        timebase.denom = 1;
    }
    platform.timebase = ((double)timebase.numer/(double)timebase.denom)*1.0e-9;
    platform.timeBase = mach_absolute_time();

    id poolClass = CLS_("NSAutoreleasePool");
    if (poolClass != nil) platform.pool = MSG_ID(MSG_ID(poolClass, SEL_("alloc")), SEL_("init"));

    // NSApplication
    platform.app = MSG_ID(CLS_("NSApplication"), SEL_("sharedApplication"));
    if (platform.app == nil)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create NSApplication");
        return -1;
    }

    MSG_BOOL_L(platform.app, SEL_("setActivationPolicy:"), NS_ACTIVATION_POLICY_REGULAR);
    MSG_VOID(platform.app, SEL_("finishLaunching"));

    RefreshDisplays();

    if (CORE.window.screen.width <= 0) CORE.window.screen.width = 800;
    if (CORE.window.screen.height <= 0) CORE.window.screen.height = 450;

    // Window
    platform.styleMask = NS_STYLE_TITLED|NS_STYLE_CLOSABLE|NS_STYLE_MINIATURIZABLE;
    if ((CORE.window.flags & FLAG_WINDOW_RESIZABLE) > 0) platform.styleMask |= NS_STYLE_RESIZABLE;
    if ((CORE.window.flags & FLAG_WINDOW_UNDECORATED) > 0) platform.styleMask = NS_STYLE_BORDERLESS;

    CGRect contentRect = CGRectMake(0.0, 0.0, (double)CORE.window.screen.width, (double)CORE.window.screen.height);

    id windowAlloc = MSG_ID(CLS_("NSWindow"), SEL_("alloc"));
    platform.window = ((id (*)(id, SEL, CGRect, unsigned long, unsigned long, BOOL))objc_msgSend)(
        windowAlloc, SEL_("initWithContentRect:styleMask:backing:defer:"),
        contentRect, platform.styleMask, NS_BACKING_BUFFERED, NO);

    if (platform.window == nil)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create NSWindow");
        return -1;
    }

    MSG_VOID_BOOL(platform.window, SEL_("setReleasedWhenClosed:"), NO);
    MSG_VOID_UL(platform.window, SEL_("setCollectionBehavior:"), NS_COLLECTION_FULLSCREEN_PRIMARY);
    MSG_VOID_BOOL(platform.window, SEL_("setAcceptsMouseMovedEvents:"), YES);
    MSG_VOID(platform.window, SEL_("center"));

    id title = NSStringFromUtf8((CORE.window.title != NULL)? CORE.window.title : "rayvulkan");
    if (title != nil) MSG_VOID_ID(platform.window, SEL_("setTitle:"), title);

    // Content view: the drop-aware subclass when it could be created
    platform.viewClass = BuildDraggingViewClass();
    Class viewClass = (platform.viewClass != NULL)? platform.viewClass : objc_getClass("NSView");
    if (viewClass == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: NSView class not available");
        return -1;
    }
    if (platform.viewClass == NULL) TRACELOG(LOG_WARNING, "PLATFORM: Drag and drop unavailable (view subclass creation failed)");

    id viewAlloc = MSG_ID((id)viewClass, SEL_("alloc"));
    platform.view = ((id (*)(id, SEL, CGRect))objc_msgSend)(viewAlloc, SEL_("initWithFrame:"), contentRect);
    if (platform.view == nil)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create content view");
        return -1;
    }

    // CAMetalLayer backing the view, this is what MoltenVK renders into
    id layerClass = CLS_("CAMetalLayer");
    if (layerClass == nil)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: CAMetalLayer class not available (QuartzCore not linked?)");
        return -1;
    }

    platform.layer = MSG_ID(MSG_ID(layerClass, SEL_("alloc")), SEL_("init"));
    if (platform.layer == nil)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create CAMetalLayer");
        return -1;
    }

    MSG_VOID_ID(platform.view, SEL_("setLayer:"), platform.layer);
    MSG_VOID_BOOL(platform.view, SEL_("setWantsLayer:"), YES);
    MSG_VOID_ID(platform.window, SEL_("setContentView:"), platform.view);
    MSG_VOID_ID(platform.window, SEL_("makeFirstResponder:"), platform.view);

    if (platform.viewClass != NULL)
    {
        id types = MSG_ID_ID(CLS_("NSArray"), SEL_("arrayWithObject:"), NSStringFromUtf8("NSFilenamesPboardType"));
        if (types != nil) MSG_VOID_ID(platform.view, SEL_("registerForDraggedTypes:"), types);
    }

    if ((CORE.window.flags & FLAG_WINDOW_TRANSPARENT) > 0)
    {
        MSG_VOID_BOOL(platform.window, SEL_("setOpaque:"), NO);
        TRACELOG(LOG_WARNING, "PLATFORM: FLAG_WINDOW_TRANSPARENT is best effort on macOS (layer stays opaque)");
    }

    if ((CORE.window.flags & FLAG_WINDOW_HIDDEN) == 0)
    {
        MSG_VOID_ID(platform.window, SEL_("makeKeyAndOrderFront:"), nil);
        MSG_VOID_BOOL(platform.app, SEL_("activateIgnoringOtherApps:"), YES);
    }

    if ((CORE.window.flags & FLAG_WINDOW_TOPMOST) > 0) MSG_VOID_UL(platform.window, SEL_("setLevel:"), 3);
    if ((CORE.window.flags & FLAG_WINDOW_MINIMIZED) > 0) MSG_VOID_ID(platform.window, SEL_("miniaturize:"), nil);
    if ((CORE.window.flags & FLAG_WINDOW_MAXIMIZED) > 0) MSG_VOID_ID(platform.window, SEL_("zoom:"), nil);

    UpdateWindowMetrics();
    platform.previousContentWidth = CORE.window.screen.width;
    platform.previousContentHeight = CORE.window.screen.height;

    // Display size of the monitor holding the window
    int monitor = GetCurrentMonitorPlatform();
    if (DisplayIsValid(monitor))
    {
        CGRect bounds = CGDisplayBounds(platform.displays[monitor]);
        CORE.window.display.width = (int)bounds.size.width;
        CORE.window.display.height = (int)bounds.size.height;
    }

    if ((CORE.window.flags & FLAG_FULLSCREEN_MODE) > 0)
    {
        CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;
        ToggleFullscreenPlatform();
    }

    // Vulkan initialization; MoltenVK requires portability enumeration (see header notes)
    static const char *instanceExtensions[] = {
        "VK_KHR_surface",
        "VK_EXT_metal_surface",
        "VK_KHR_portability_enumeration"
    };

    rlvkPlatformGlue glue = {
        .requiredExtensions = instanceExtensions,
        .requiredExtensionCount = 3,
        .createSurface = CreateSurfaceMetal,
        .userData = NULL,
        .portability = true
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

    TRACELOG(LOG_INFO, "PLATFORM: Initialized successfully (macOS/Cocoa)");
    TRACELOG(LOG_INFO, "    > Window size:   %i x %i", CORE.window.screen.width, CORE.window.screen.height);
    TRACELOG(LOG_INFO, "    > Render size:   %i x %i", CORE.window.render.width, CORE.window.render.height);
    TRACELOG(LOG_INFO, "    > Display size:  %i x %i", CORE.window.display.width, CORE.window.display.height);
    TRACELOG(LOG_INFO, "    > Monitors:      %i", platform.displayCount);

    return 0;
}

void ClosePlatform(void)
{
    if (CORE.input.mouse.cursorLocked)
    {
        CGAssociateMouseAndMouseCursorPosition(true);
        CORE.input.mouse.cursorLocked = false;
    }

    if (CORE.input.mouse.cursorHidden)
    {
        MSG_VOID(CLS_("NSCursor"), SEL_("unhide"));
        CORE.input.mouse.cursorHidden = false;
    }

    if (platform.window != nil)
    {
        MSG_VOID_ID(platform.window, SEL_("orderOut:"), nil);
        MSG_VOID(platform.window, SEL_("close"));
        platform.window = nil;
    }

    platform.view = nil;
    platform.layer = nil;

    if (platform.clipboardText != NULL)
    {
        RL_FREE(platform.clipboardText);
        platform.clipboardText = NULL;
    }

    if (platform.pool != nil)
    {
        MSG_VOID(platform.pool, SEL_("drain"));
        platform.pool = nil;
    }

    TRACELOG(LOG_INFO, "PLATFORM: Closed (macOS)");
}

//----------------------------------------------------------------------------------
// Platform contract: event pump and timing
//----------------------------------------------------------------------------------
void PollInputEventsPlatform(void)
{
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

    if (platform.app == nil) return;

    id pool = MSG_ID(MSG_ID(CLS_("NSAutoreleasePool"), SEL_("alloc")), SEL_("init"));

    id distantPast = MSG_ID(CLS_("NSDate"), SEL_("distantPast"));
    id untilDate = distantPast;
    if (CORE.window.eventWaiting) untilDate = MSG_ID(CLS_("NSDate"), SEL_("distantFuture"));

    id runLoopMode = NSStringFromUtf8("kCFRunLoopDefaultMode");

    for (;;)
    {
        id event = ((id (*)(id, SEL, unsigned long long, id, id, BOOL))objc_msgSend)(
            platform.app, SEL_("nextEventMatchingMask:untilDate:inMode:dequeue:"),
            NS_EVENT_MASK_ANY, untilDate, runLoopMode, YES);

        if (event == nil) break;

        ProcessEvent(event);
        untilDate = distantPast;            // Only the first fetch may block
    }

    // Window closed by the user (there is no delegate, the state is polled)
    if ((platform.window != nil) && (MSG_BOOL(platform.window, SEL_("isVisible")) == NO) &&
        ((CORE.window.flags & (FLAG_WINDOW_HIDDEN|FLAG_WINDOW_MINIMIZED)) == 0))
    {
        CORE.window.shouldClose = true;
    }

    // Focus state
    if (platform.window != nil)
    {
        bool focused = (MSG_BOOL(platform.window, SEL_("isKeyWindow")) != NO);
        bool wasFocused = ((CORE.window.flags & FLAG_WINDOW_UNFOCUSED) == 0);

        if (focused) CORE.window.flags &= ~FLAG_WINDOW_UNFOCUSED;
        else CORE.window.flags |= FLAG_WINDOW_UNFOCUSED;

        if (wasFocused && !focused) ResetInputStates();

        if (MSG_BOOL(platform.window, SEL_("isMiniaturized")) != NO) CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
        else CORE.window.flags &= ~FLAG_WINDOW_MINIMIZED;
    }

    // Resize detection (live resizes and fullscreen transitions are animated)
    if (platform.view != nil)
    {
        CGRect content = MsgRect(platform.view, SEL_("frame"));
        int width = (int)content.size.width;
        int height = (int)content.size.height;

        if ((width != platform.previousContentWidth) || (height != platform.previousContentHeight))
        {
            platform.previousContentWidth = width;
            platform.previousContentHeight = height;

            UpdateWindowMetrics();
            CORE.window.resizedLastFrame = true;
            if (rlvkIsReady()) rlvkResize(CORE.window.render.width, CORE.window.render.height);
        }
    }

    ApplyCursorShape();

    if (pool != nil) MSG_VOID(pool, SEL_("drain"));
}

double GetTimePlatform(void)
{
    uint64_t ticks = mach_absolute_time();
    return (double)ticks*platform.timebase;
}

void WaitTimePlatform(double seconds)
{
    if (seconds <= 0.0) return;

    double destination = GetTimePlatform() + seconds;

    double sleepSeconds = seconds*0.95;
    if (sleepSeconds > 0.0)
    {
        struct timespec request;
        request.tv_sec = (time_t)sleepSeconds;
        request.tv_nsec = (long)((sleepSeconds - (double)request.tv_sec)*1.0e9);

        struct timespec remaining;
        while ((nanosleep(&request, &remaining) == -1) && (errno == EINTR)) request = remaining;
    }

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
    if (platform.window == nil) return;

    CORE.window.title = title;

    id string = NSStringFromUtf8((title != NULL)? title : "");
    if (string != nil) MSG_VOID_ID(platform.window, SEL_("setTitle:"), string);
}

void SetWindowPositionPlatform(int x, int y)
{
    if (platform.window == nil) return;

    // raylib positions are top-left based, Cocoa screen coordinates are bottom-left based
    CGRect frame = MsgRect(platform.window, SEL_("frame"));
    CGRect primary = CGDisplayBounds(CGMainDisplayID());

    CGPoint origin;
    origin.x = (double)x;
    origin.y = primary.size.height - (double)y - frame.size.height;

    MSG_VOID_PT(platform.window, SEL_("setFrameOrigin:"), origin);

    CORE.window.position.x = x;
    CORE.window.position.y = y;
}

void SetWindowSizePlatform(int width, int height)
{
    if ((platform.window == nil) || (width <= 0) || (height <= 0)) return;

    CGSize size;
    size.width = (double)width;
    size.height = (double)height;

    MSG_VOID_SZ(platform.window, SEL_("setContentSize:"), size);
    UpdateWindowMetrics();

    platform.previousContentWidth = CORE.window.screen.width;
    platform.previousContentHeight = CORE.window.screen.height;
    if (rlvkIsReady()) rlvkResize(CORE.window.render.width, CORE.window.render.height);
}

void SetWindowMinSizePlatform(int width, int height)
{
    CORE.window.screenMin.width = width;
    CORE.window.screenMin.height = height;

    if (platform.window == nil) return;

    CGSize size;
    size.width = (double)((width > 0)? width : 0);
    size.height = (double)((height > 0)? height : 0);
    MSG_VOID_SZ(platform.window, SEL_("setContentMinSize:"), size);
}

void SetWindowMaxSizePlatform(int width, int height)
{
    CORE.window.screenMax.width = width;
    CORE.window.screenMax.height = height;

    if (platform.window == nil) return;

    // Zero or negative means "no limit", Cocoa expresses that with a huge value
    CGSize size;
    size.width = (width > 0)? (double)width : 1.0e7;
    size.height = (height > 0)? (double)height : 1.0e7;
    MSG_VOID_SZ(platform.window, SEL_("setContentMaxSize:"), size);
}

void SetWindowOpacityPlatform(float opacity)
{
    if (platform.window == nil) return;

    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;

    MSG_VOID_DBL(platform.window, SEL_("setAlphaValue:"), (double)opacity);
}

void SetWindowIconPlatform(Image *images, int count)
{
    (void)images;
    (void)count;

    // macOS takes the application icon from the bundle Info.plist; a window has no icon
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowIcon() has no effect on macOS (icon comes from the app bundle)");
}

void SetWindowFocusedPlatform(void)
{
    if (platform.window == nil) return;

    MSG_VOID_ID(platform.window, SEL_("makeKeyAndOrderFront:"), nil);
    if (platform.app != nil) MSG_VOID_BOOL(platform.app, SEL_("activateIgnoringOtherApps:"), YES);
}

void SetWindowMonitorPlatform(int monitor)
{
    if (platform.window == nil) return;

    RefreshDisplays();
    if (!DisplayIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return;
    }

    CGRect bounds = CGDisplayBounds(platform.displays[monitor]);
    CGRect frame = MsgRect(platform.window, SEL_("frame"));
    CGRect primary = CGDisplayBounds(CGMainDisplayID());

    // Center the window on the requested display (CGDisplayBounds is top-left based)
    double centerX = bounds.origin.x + (bounds.size.width - frame.size.width)*0.5;
    double topY = bounds.origin.y + (bounds.size.height - frame.size.height)*0.5;

    CGPoint origin;
    origin.x = centerX;
    origin.y = primary.size.height - topY - frame.size.height;
    MSG_VOID_PT(platform.window, SEL_("setFrameOrigin:"), origin);

    CORE.window.display.width = (int)bounds.size.width;
    CORE.window.display.height = (int)bounds.size.height;

    UpdateWindowMetrics();
}

void SetWindowStatePlatform(unsigned int flags)
{
    if (platform.window == nil) return;

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

    if ((flags & FLAG_WINDOW_RESIZABLE) > 0)
    {
        platform.styleMask |= NS_STYLE_RESIZABLE;
        MSG_VOID_UL(platform.window, SEL_("setStyleMask:"), platform.styleMask);
    }

    if ((flags & FLAG_WINDOW_UNDECORATED) > 0)
    {
        platform.styleMask = NS_STYLE_BORDERLESS;
        MSG_VOID_UL(platform.window, SEL_("setStyleMask:"), platform.styleMask);
    }

    if ((flags & FLAG_WINDOW_HIDDEN) > 0) MSG_VOID_ID(platform.window, SEL_("orderOut:"), nil);
    if ((flags & FLAG_WINDOW_MINIMIZED) > 0) MSG_VOID_ID(platform.window, SEL_("miniaturize:"), nil);
    if ((flags & FLAG_WINDOW_MAXIMIZED) > 0) MSG_VOID_ID(platform.window, SEL_("zoom:"), nil);
    if ((flags & FLAG_WINDOW_TOPMOST) > 0) MSG_VOID_UL(platform.window, SEL_("setLevel:"), 3);
    if ((flags & FLAG_WINDOW_MOUSE_PASSTHROUGH) > 0) MSG_VOID_BOOL(platform.window, SEL_("setIgnoresMouseEvents:"), YES);
    if ((flags & FLAG_MSAA_4X_HINT) > 0) TRACELOG(LOG_WARNING, "PLATFORM: MSAA can only be configured before window creation");
    if ((flags & FLAG_INTERLACED_HINT) > 0) TRACELOG(LOG_WARNING, "PLATFORM: FLAG_INTERLACED_HINT not supported on macOS");
}

void ClearWindowStatePlatform(unsigned int flags)
{
    if (platform.window == nil) return;

    if ((flags & FLAG_VSYNC_HINT) > 0) rlvkSetVSync(false);
    if (((flags & FLAG_FULLSCREEN_MODE) > 0) && CORE.window.fullscreen) ToggleFullscreenPlatform();
    if (((flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0) &&
        ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) > 0)) ToggleBorderlessWindowedPlatform();

    CORE.window.flags &= ~flags;

    if ((flags & FLAG_WINDOW_RESIZABLE) > 0)
    {
        platform.styleMask &= ~NS_STYLE_RESIZABLE;
        MSG_VOID_UL(platform.window, SEL_("setStyleMask:"), platform.styleMask);
    }

    if ((flags & FLAG_WINDOW_UNDECORATED) > 0)
    {
        platform.styleMask = NS_STYLE_TITLED|NS_STYLE_CLOSABLE|NS_STYLE_MINIATURIZABLE;
        if ((CORE.window.flags & FLAG_WINDOW_RESIZABLE) > 0) platform.styleMask |= NS_STYLE_RESIZABLE;
        MSG_VOID_UL(platform.window, SEL_("setStyleMask:"), platform.styleMask);
    }

    if ((flags & FLAG_WINDOW_HIDDEN) > 0) MSG_VOID_ID(platform.window, SEL_("makeKeyAndOrderFront:"), nil);
    if ((flags & FLAG_WINDOW_MINIMIZED) > 0) MSG_VOID_ID(platform.window, SEL_("deminiaturize:"), nil);
    if ((flags & FLAG_WINDOW_MAXIMIZED) > 0) MSG_VOID_ID(platform.window, SEL_("zoom:"), nil);
    if ((flags & FLAG_WINDOW_TOPMOST) > 0) MSG_VOID_UL(platform.window, SEL_("setLevel:"), 0);
    if ((flags & FLAG_WINDOW_MOUSE_PASSTHROUGH) > 0) MSG_VOID_BOOL(platform.window, SEL_("setIgnoresMouseEvents:"), NO);
}

void ToggleFullscreenPlatform(void)
{
    if (platform.window == nil) return;

    MSG_VOID_ID(platform.window, SEL_("toggleFullScreen:"), nil);

    CORE.window.fullscreen = !CORE.window.fullscreen;
    if (CORE.window.fullscreen) CORE.window.flags |= FLAG_FULLSCREEN_MODE;
    else CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;

    TRACELOG(LOG_INFO, "PLATFORM: %s fullscreen mode", CORE.window.fullscreen? "Entered" : "Left");
    // NOTE: The transition is animated; the new size is picked up by the resize polling
}

void ToggleBorderlessWindowedPlatform(void)
{
    if (platform.window == nil) return;

    if ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) == 0)
    {
        CORE.window.previousPosition.x = CORE.window.position.x;
        CORE.window.previousPosition.y = CORE.window.position.y;
        CORE.window.previousScreen.width = CORE.window.screen.width;
        CORE.window.previousScreen.height = CORE.window.screen.height;

        RefreshDisplays();
        int monitor = GetCurrentMonitorPlatform();
        if (!DisplayIsValid(monitor))
        {
            TRACELOG(LOG_WARNING, "PLATFORM: No display available for borderless windowed mode");
            return;
        }

        CGRect bounds = CGDisplayBounds(platform.displays[monitor]);
        CGRect primary = CGDisplayBounds(CGMainDisplayID());

        platform.styleMask = NS_STYLE_BORDERLESS;
        MSG_VOID_UL(platform.window, SEL_("setStyleMask:"), platform.styleMask);

        CGRect frame = CGRectMake(bounds.origin.x,
                                  primary.size.height - (bounds.origin.y + bounds.size.height),
                                  bounds.size.width, bounds.size.height);
        ((void (*)(id, SEL, CGRect, BOOL))objc_msgSend)(platform.window, SEL_("setFrame:display:"), frame, YES);

        CORE.window.flags |= FLAG_BORDERLESS_WINDOWED_MODE;
        TRACELOG(LOG_INFO, "PLATFORM: Entered borderless windowed mode");
    }
    else
    {
        platform.styleMask = NS_STYLE_TITLED|NS_STYLE_CLOSABLE|NS_STYLE_MINIATURIZABLE;
        if ((CORE.window.flags & FLAG_WINDOW_RESIZABLE) > 0) platform.styleMask |= NS_STYLE_RESIZABLE;
        MSG_VOID_UL(platform.window, SEL_("setStyleMask:"), platform.styleMask);

        CORE.window.flags &= ~FLAG_BORDERLESS_WINDOWED_MODE;

        SetWindowSizePlatform(CORE.window.previousScreen.width, CORE.window.previousScreen.height);
        SetWindowPositionPlatform(CORE.window.previousPosition.x, CORE.window.previousPosition.y);
        TRACELOG(LOG_INFO, "PLATFORM: Left borderless windowed mode");
    }

    UpdateWindowMetrics();
}

void MaximizeWindowPlatform(void)
{
    if (platform.window == nil) return;

    if (MSG_BOOL(platform.window, SEL_("isZoomed")) == NO) MSG_VOID_ID(platform.window, SEL_("zoom:"), nil);
    CORE.window.flags |= FLAG_WINDOW_MAXIMIZED;
}

void MinimizeWindowPlatform(void)
{
    if (platform.window == nil) return;

    MSG_VOID_ID(platform.window, SEL_("miniaturize:"), nil);
    CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
}

void RestoreWindowPlatform(void)
{
    if (platform.window == nil) return;

    if (MSG_BOOL(platform.window, SEL_("isMiniaturized")) != NO) MSG_VOID_ID(platform.window, SEL_("deminiaturize:"), nil);
    else if (MSG_BOOL(platform.window, SEL_("isZoomed")) != NO) MSG_VOID_ID(platform.window, SEL_("zoom:"), nil);

    CORE.window.flags &= ~(FLAG_WINDOW_MINIMIZED|FLAG_WINDOW_MAXIMIZED);
    UpdateWindowMetrics();
}

void *GetWindowHandlePlatform(void)
{
    return (void *)platform.window;
}

//----------------------------------------------------------------------------------
// Platform contract: monitors
//----------------------------------------------------------------------------------
int GetMonitorCountPlatform(void)
{
    RefreshDisplays();
    return platform.displayCount;
}

int GetCurrentMonitorPlatform(void)
{
    if (platform.displayCount == 0) RefreshDisplays();
    if ((platform.window == nil) || (platform.displayCount == 0)) return 0;

    CGRect frame = MsgRect(platform.window, SEL_("frame"));
    CGRect primary = CGDisplayBounds(CGMainDisplayID());

    // Window center in the top-left based CoreGraphics space
    CGPoint center;
    center.x = frame.origin.x + frame.size.width*0.5;
    center.y = primary.size.height - (frame.origin.y + frame.size.height*0.5);

    for (int i = 0; i < platform.displayCount; i++)
    {
        if (CGRectContainsPoint(CGDisplayBounds(platform.displays[i]), center)) return i;
    }

    return 0;
}

Vector2 GetMonitorPositionPlatform(int monitor)
{
    Vector2 position = { 0.0f, 0.0f };

    RefreshDisplays();
    if (!DisplayIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return position;
    }

    CGRect bounds = CGDisplayBounds(platform.displays[monitor]);
    position.x = (float)bounds.origin.x;
    position.y = (float)bounds.origin.y;

    return position;
}

int GetMonitorWidthPlatform(int monitor)
{
    RefreshDisplays();
    if (!DisplayIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return (int)CGDisplayBounds(platform.displays[monitor]).size.width;
}

int GetMonitorHeightPlatform(int monitor)
{
    RefreshDisplays();
    if (!DisplayIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return (int)CGDisplayBounds(platform.displays[monitor]).size.height;
}

int GetMonitorPhysicalWidthPlatform(int monitor)
{
    RefreshDisplays();
    if (!DisplayIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return (int)CGDisplayScreenSize(platform.displays[monitor]).width;
}

int GetMonitorPhysicalHeightPlatform(int monitor)
{
    RefreshDisplays();
    if (!DisplayIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return (int)CGDisplayScreenSize(platform.displays[monitor]).height;
}

int GetMonitorRefreshRatePlatform(int monitor)
{
    RefreshDisplays();
    if (!DisplayIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(platform.displays[monitor]);
    if (mode == NULL) return 0;

    double refresh = CGDisplayModeGetRefreshRate(mode);
    CGDisplayModeRelease(mode);

    // Built-in panels report 0: fall back to the common 60 Hz to keep SetTargetFPS() sane
    if (refresh <= 0.0) return 60;

    return (int)(refresh + 0.5);
}

const char *GetMonitorNamePlatform(int monitor)
{
    platform.monitorName[0] = '\0';

    RefreshDisplays();
    if (!DisplayIsValid(monitor))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return platform.monitorName;
    }

    CGDirectDisplayID target = platform.displays[monitor];

    // NSScreen.localizedName (macOS 10.15+) gives the human readable panel name
    id screens = MSG_ID(CLS_("NSScreen"), SEL_("screens"));
    if (screens != nil)
    {
        unsigned long count = MSG_ULONG(screens, SEL_("count"));
        for (unsigned long i = 0; i < count; i++)
        {
            id screen = MSG_ID_IDX(screens, SEL_("objectAtIndex:"), i);
            if (screen == nil) continue;

            id description = MSG_ID(screen, SEL_("deviceDescription"));
            if (description == nil) continue;

            id number = MSG_ID_ID(description, SEL_("objectForKey:"), NSStringFromUtf8("NSScreenNumber"));
            if (number == nil) continue;
            if ((CGDirectDisplayID)MSG_UINT(number, SEL_("unsignedIntValue")) != target) continue;

            if (!MSG_BOOL_SEL(screen, SEL_("respondsToSelector:"), SEL_("localizedName"))) break;

            id name = MSG_ID(screen, SEL_("localizedName"));
            const char *utf8 = (name != nil)? MSG_CSTR(name, SEL_("UTF8String")) : NULL;
            if (utf8 != NULL)
            {
                strncpy(platform.monitorName, utf8, sizeof(platform.monitorName) - 1);
                platform.monitorName[sizeof(platform.monitorName) - 1] = '\0';
                return platform.monitorName;
            }

            break;
        }
    }

    // Fallback: synthesize a stable name
    TextCopy(platform.monitorName, CGDisplayIsBuiltin(target)? "Built-in Display" : "External Display");

    return platform.monitorName;
}

Vector2 GetWindowScaleDPIPlatform(void)
{
    Vector2 scale = { 1.0f, 1.0f };

    if (platform.window != nil)
    {
        double backing = MSG_DBL(platform.window, SEL_("backingScaleFactor"));
        if (backing > 0.0)
        {
            scale.x = (float)backing;
            scale.y = (float)backing;
        }
    }

    return scale;
}

//----------------------------------------------------------------------------------
// Platform contract: cursor
//----------------------------------------------------------------------------------
void ShowCursorPlatform(void)
{
    if (CORE.input.mouse.cursorHidden) MSG_VOID(CLS_("NSCursor"), SEL_("unhide"));
    CORE.input.mouse.cursorHidden = false;
    ApplyCursorShape();
}

void HideCursorPlatform(void)
{
    if (!CORE.input.mouse.cursorHidden) MSG_VOID(CLS_("NSCursor"), SEL_("hide"));
    CORE.input.mouse.cursorHidden = true;
}

void EnableCursorPlatform(void)
{
    if (CORE.input.mouse.cursorLocked) CGAssociateMouseAndMouseCursorPosition(true);
    if (CORE.input.mouse.cursorHidden) MSG_VOID(CLS_("NSCursor"), SEL_("unhide"));

    CORE.input.mouse.cursorLocked = false;
    CORE.input.mouse.cursorHidden = false;
    ApplyCursorShape();
}

void DisableCursorPlatform(void)
{
    if (platform.window == nil) return;

    // Warp to the window center first so the cursor reappears in a sane spot later
    CGRect frame = MsgRect(platform.window, SEL_("frame"));
    CGRect primary = CGDisplayBounds(CGMainDisplayID());

    CGPoint center;
    center.x = frame.origin.x + frame.size.width*0.5;
    center.y = primary.size.height - (frame.origin.y + frame.size.height*0.5);
    CGWarpMouseCursorPosition(center);

    CGAssociateMouseAndMouseCursorPosition(false);
    if (!CORE.input.mouse.cursorHidden) MSG_VOID(CLS_("NSCursor"), SEL_("hide"));

    CORE.input.mouse.cursorLocked = true;
    CORE.input.mouse.cursorHidden = true;
}

void SetMouseCursorPlatform(int cursor)
{
    if ((cursor < 0) || (cursor > MOUSE_CURSOR_NOT_ALLOWED))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Unsupported mouse cursor shape (%i)", cursor);
        return;
    }

    CORE.input.mouse.cursor = cursor;
    ApplyCursorShape();
}

void SetMousePositionPlatform(int x, int y)
{
    CORE.input.mouse.currentPosition.x = (float)x;
    CORE.input.mouse.currentPosition.y = (float)y;
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;

    if (platform.window == nil) return;

    CGRect frame = MsgRect(platform.window, SEL_("frame"));
    CGRect content = MsgRect(platform.view, SEL_("frame"));
    CGRect primary = CGDisplayBounds(CGMainDisplayID());

    // Content origin in top-left based global coordinates
    double contentTop = primary.size.height - (frame.origin.y + frame.size.height);
    double titleBar = frame.size.height - content.size.height;

    CGPoint point;
    point.x = frame.origin.x + (double)x;
    point.y = contentTop + titleBar + (double)y;

    CGWarpMouseCursorPosition(point);
    if (!CORE.input.mouse.cursorLocked) CGAssociateMouseAndMouseCursorPosition(true);
}

//----------------------------------------------------------------------------------
// Platform contract: clipboard
//----------------------------------------------------------------------------------
void SetClipboardTextPlatform(const char *text)
{
    if (text == NULL) return;

    id pasteboard = MSG_ID(CLS_("NSPasteboard"), SEL_("generalPasteboard"));
    if (pasteboard == nil)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Clipboard not available");
        return;
    }

    id type = NSStringFromUtf8("public.utf8-plain-text");
    id string = NSStringFromUtf8(text);
    if ((type == nil) || (string == nil)) return;

    id types = MSG_ID_ID(CLS_("NSArray"), SEL_("arrayWithObject:"), type);
    ((long (*)(id, SEL, id, id))objc_msgSend)(pasteboard, SEL_("declareTypes:owner:"), types, nil);

    if (((BOOL (*)(id, SEL, id, id))objc_msgSend)(pasteboard, SEL_("setString:forType:"), string, type) == NO)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to write clipboard text");
    }
}

const char *GetClipboardTextPlatform(void)
{
    if (platform.clipboardText != NULL)
    {
        RL_FREE(platform.clipboardText);
        platform.clipboardText = NULL;
    }

    id pasteboard = MSG_ID(CLS_("NSPasteboard"), SEL_("generalPasteboard"));
    if (pasteboard == nil) return NULL;

    id type = NSStringFromUtf8("public.utf8-plain-text");
    id string = MSG_ID_ID(pasteboard, SEL_("stringForType:"), type);
    if (string == nil)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Clipboard does not contain readable text");
        return NULL;
    }

    const char *utf8 = MSG_CSTR(string, SEL_("UTF8String"));
    if (utf8 == NULL) return NULL;

    size_t length = strlen(utf8);
    platform.clipboardText = (char *)RL_CALLOC(length + 1, 1);
    if (platform.clipboardText == NULL) return NULL;

    memcpy(platform.clipboardText, utf8, length);

    return platform.clipboardText;
}

Image GetClipboardImagePlatform(void)
{
    Image image = { 0 };

    id pasteboard = MSG_ID(CLS_("NSPasteboard"), SEL_("generalPasteboard"));
    if (pasteboard == nil) return image;

    // Only PNG is requested: it is lossless and decodable by the bundled stb_image
    id data = MSG_ID_ID(pasteboard, SEL_("dataForType:"), NSStringFromUtf8("public.png"));
    if (data != nil)
    {
        const void *bytes = MSG_PTR(data, SEL_("bytes"));
        unsigned long length = MSG_ULONG(data, SEL_("length"));

        if ((bytes != NULL) && (length > 0))
        {
            image = LoadImageFromMemory(".png", (const unsigned char *)bytes, (int)length);
            if (image.data != NULL) return image;
        }
    }

    TRACELOG(LOG_WARNING, "PLATFORM: Clipboard does not contain a PNG image");

    return image;
}

//----------------------------------------------------------------------------------
// Platform contract: misc
//----------------------------------------------------------------------------------
void OpenURLPlatform(const char *url)
{
    if (url == NULL) return;

    if (strchr(url, '\"') != NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Provided URL contains invalid characters, not opened");
        return;
    }

    id string = NSStringFromUtf8(url);
    if (string == nil) return;

    id nsurl = MSG_ID_ID(CLS_("NSURL"), SEL_("URLWithString:"), string);
    if (nsurl == nil)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Provided URL is not valid");
        return;
    }

    id workspace = MSG_ID(CLS_("NSWorkspace"), SEL_("sharedWorkspace"));
    if (workspace == nil) return;

    if (MSG_BOOL_ID(workspace, SEL_("openURL:"), nsurl) == NO) TRACELOG(LOG_WARNING, "PLATFORM: Failed to open URL");
}

void SetGamepadVibrationPlatform(int gamepad, float leftMotor, float rightMotor, float duration)
{
    (void)gamepad;
    (void)leftMotor;
    (void)rightMotor;
    (void)duration;

    // Gamepad support needs the block based GameController framework, see the header notes
    TRACELOG(LOG_WARNING, "PLATFORM: Gamepad support is not implemented on the macOS backend");
}

int SetGamepadMappingsPlatform(const char *mappings)
{
    (void)mappings;

    TRACELOG(LOG_WARNING, "PLATFORM: Gamepad support is not implemented on the macOS backend");

    return 0;
}
