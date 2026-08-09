/**********************************************************************************************
*
*   rcore_wayland - raylib core platform backend for Linux/Wayland (raw libwayland-client)
*
*   Implements the platform contract declared in src/rcore_internal.h on top of
*   libwayland-client, xdg-shell, xdg-decoration-unstable-v1, relative-pointer-unstable-v1,
*   pointer-constraints-unstable-v1, libwayland-cursor, libxkbcommon (keymap handling,
*   the Wayland equivalent of what Xlib does for us on X11) and VK_KHR_wayland_surface.
*
*   CONTRACT NOTES (additions/clarifications over src/rcore_internal.h):
*     - PollInputEventsPlatform() performs the per-frame input bookkeeping itself
*       (previous<-current state copies, queue resets) *before* pumping native events.
*       The operations are idempotent so a caller doing the same beforehand is harmless.
*     - GetWindowHandlePlatform() returns the struct wl_surface* of the toplevel.
*
*   NOT SUPPORTED BY THE WAYLAND PROTOCOL (logged as warnings, calls are safe no-ops):
*     window opacity, absolute window positioning (SetWindowPosition/GetWindowPosition
*     stay at 0,0), window icons, self-focusing, warping the pointer while unlocked,
*     FLAG_WINDOW_TOPMOST, FLAG_WINDOW_MOUSE_PASSTHROUGH, FLAG_INTERLACED_HINT.
*     When the compositor has no server-side decoration manager the window is drawn
*     undecorated (no client-side decorations are painted) and a warning is logged.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#ifndef _GNU_SOURCE
    #define _GNU_SOURCE     // clock_gettime/nanosleep/glob/pipe2 visibility under -std=c99
#endif

#include "rcore_internal.h"
#include "rcore_linux_common.h"

#define VK_USE_PLATFORM_WAYLAND_KHR
#include "volk.h"

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

#define MAX_MONITORS            8
#define MIME_TEXT               "text/plain;charset=utf-8"
#define MIME_URI_LIST           "text/uri-list"
#define MIME_IMAGE_PNG          "image/png"
#define TRANSFER_TIMEOUT_MS     1000

//----------------------------------------------------------------------------------
// Types and global platform state
//----------------------------------------------------------------------------------
typedef struct MonitorData {
    struct wl_output *output;
    int x, y;
    int width, height;              // Current mode size, in physical pixels
    int physicalWidth, physicalHeight;
    int refreshRate;
    int scale;
    char name[64];
} MonitorData;

typedef struct PlatformData {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_data_device_manager *dataDeviceManager;
    struct wl_data_device *dataDevice;
    struct xdg_wm_base *wmBase;
    struct zxdg_decoration_manager_v1 *decorationManager;
    struct zwp_relative_pointer_manager_v1 *relativePointerManager;
    struct zwp_pointer_constraints_v1 *pointerConstraints;

    struct wl_surface *surface;
    struct xdg_surface *xdgSurface;
    struct xdg_toplevel *xdgToplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct zwp_relative_pointer_v1 *relativePointer;
    struct zwp_locked_pointer_v1 *lockedPointer;

    struct wl_cursor_theme *cursorTheme;
    struct wl_surface *cursorSurface;
    int cursorThemeSize;

    struct xkb_context *xkbContext;
    struct xkb_keymap *xkbKeymap;
    struct xkb_state *xkbState;

    MonitorData monitors[MAX_MONITORS];
    int monitorCount;
    struct wl_output *currentOutput;

    // Serials required by the protocol
    uint32_t pointerEnterSerial;
    uint32_t lastInputSerial;

    // Pending toplevel configure state
    int pendingWidth, pendingHeight;
    bool pendingConfigure;
    bool pendingFullscreen;
    bool pendingMaximized;
    bool pendingActivated;
    bool closeRequested;

    // Key repeat emulation (compositor only reports rate/delay)
    int32_t repeatRate, repeatDelay;
    int repeatKey;                  // raylib key currently repeating, 0 = none
    uint32_t repeatScancode;
    double repeatNextTime;

    // Clipboard / drag and drop
    struct wl_data_offer *selectionOffer;
    struct wl_data_offer *dragOffer;
    bool dragOfferHasUriList;
    struct wl_data_source *selectionSource;
    char *selectionOwnedText;       // Text we advertise as selection owner
    char *clipboardFetched;         // Buffer returned by GetClipboardTextPlatform()

    bool serverSideDecorations;
    bool ready;
} PlatformData;

static PlatformData platform = { 0 };

//----------------------------------------------------------------------------------
// Module internal functions declaration
//----------------------------------------------------------------------------------
static void UpdateWindowSize(int width, int height);
static void ClearDroppedFiles(void);
static void StoreDroppedUriList(const char *uriList, size_t size);
static char *ReceiveOfferData(struct wl_data_offer *offer, const char *mime, size_t *outSize);
static int TranslateKeysym(xkb_keysym_t sym);
static void SetCursorShape(int cursor);
static uint64_t CreateSurfaceWayland(void *instance, void *userData);

//----------------------------------------------------------------------------------
// Small helpers
//----------------------------------------------------------------------------------

// True when the toplevel window exists
static inline bool WindowValid(void) { return (platform.display != NULL) && (platform.xdgToplevel != NULL); }

// Push every codepoint of a UTF-8 buffer into the raylib char queue
static void PushCharsUtf8(const char *text, int length)
{
    int i = 0;
    while (i < length)
    {
        unsigned char c = (unsigned char)text[i];
        int extra = 0;
        int codepoint = 0;

        if (c == 0) break;
        else if (c < 0x80) { codepoint = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { codepoint = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { codepoint = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { codepoint = c & 0x07; extra = 3; }
        else { i++; continue; }

        if ((i + extra) >= length) break;
        for (int k = 1; k <= extra; k++) codepoint = (codepoint << 6) | ((unsigned char)text[i + k] & 0x3F);
        i += extra + 1;

        if (codepoint < 32) continue;    // Control characters are not text input
        if (CORE.input.keyboard.charPressedQueueCount >= MAX_CHAR_PRESSED_QUEUE) break;
        CORE.input.keyboard.charPressedQueue[CORE.input.keyboard.charPressedQueueCount] = codepoint;
        CORE.input.keyboard.charPressedQueueCount++;
    }
}

// Queue a key press, honoring the raylib pressed-key queue capacity
static void QueueKeyPressed(int key, bool repeat)
{
    if ((key <= 0) || (key >= MAX_KEYBOARD_KEYS)) return;

    if (repeat) CORE.input.keyboard.keyRepeatInFrame[key] = 1;
    CORE.input.keyboard.currentKeyState[key] = 1;

    if (CORE.input.keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
    {
        CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount] = key;
        CORE.input.keyboard.keyPressedQueueCount++;
    }
}

// Apply a new window size to CORE and the renderer
static void UpdateWindowSize(int width, int height)
{
    if ((width <= 0) || (height <= 0)) return;

    int renderWidth = (int)((float)width*CORE.window.scale.x + 0.5f);
    int renderHeight = (int)((float)height*CORE.window.scale.y + 0.5f);
    if (renderWidth <= 0) renderWidth = width;
    if (renderHeight <= 0) renderHeight = height;

    if ((width == CORE.window.screen.width) && (height == CORE.window.screen.height) &&
        (renderWidth == CORE.window.render.width) && (renderHeight == CORE.window.render.height)) return;

    CORE.window.screen.width = width;
    CORE.window.screen.height = height;
    CORE.window.render.width = renderWidth;
    CORE.window.render.height = renderHeight;
    CORE.window.resizedLastFrame = true;

    if (platform.xdgSurface != NULL) xdg_surface_set_window_geometry(platform.xdgSurface, 0, 0, width, height);
    if (rlvkIsReady()) rlvkResize(renderWidth, renderHeight);
}

//----------------------------------------------------------------------------------
// Keyboard mapping (xkbcommon keysyms share the X11 keysym numbering)
//----------------------------------------------------------------------------------

// Translate an xkb keysym to a raylib KeyboardKey, KEY_NULL when unmapped
static int TranslateKeysym(xkb_keysym_t sym)
{
    if ((sym >= XKB_KEY_a) && (sym <= XKB_KEY_z)) return KEY_A + (int)(sym - XKB_KEY_a);
    if ((sym >= XKB_KEY_A) && (sym <= XKB_KEY_Z)) return KEY_A + (int)(sym - XKB_KEY_A);
    if ((sym >= XKB_KEY_0) && (sym <= XKB_KEY_9)) return KEY_ZERO + (int)(sym - XKB_KEY_0);
    if ((sym >= XKB_KEY_F1) && (sym <= XKB_KEY_F12)) return KEY_F1 + (int)(sym - XKB_KEY_F1);
    if ((sym >= XKB_KEY_KP_0) && (sym <= XKB_KEY_KP_9)) return KEY_KP_0 + (int)(sym - XKB_KEY_KP_0);

    switch (sym)
    {
        case XKB_KEY_space: return KEY_SPACE;
        case XKB_KEY_apostrophe: return KEY_APOSTROPHE;
        case XKB_KEY_comma: return KEY_COMMA;
        case XKB_KEY_minus: return KEY_MINUS;
        case XKB_KEY_period: return KEY_PERIOD;
        case XKB_KEY_slash: return KEY_SLASH;
        case XKB_KEY_semicolon: return KEY_SEMICOLON;
        case XKB_KEY_equal: return KEY_EQUAL;
        case XKB_KEY_bracketleft: return KEY_LEFT_BRACKET;
        case XKB_KEY_backslash: return KEY_BACKSLASH;
        case XKB_KEY_bracketright: return KEY_RIGHT_BRACKET;
        case XKB_KEY_grave: return KEY_GRAVE;
        case XKB_KEY_Escape: return KEY_ESCAPE;
        case XKB_KEY_Return: return KEY_ENTER;
        case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return KEY_TAB;
        case XKB_KEY_BackSpace: return KEY_BACKSPACE;
        case XKB_KEY_Insert: return KEY_INSERT;
        case XKB_KEY_Delete: return KEY_DELETE;
        case XKB_KEY_Right: return KEY_RIGHT;
        case XKB_KEY_Left: return KEY_LEFT;
        case XKB_KEY_Down: return KEY_DOWN;
        case XKB_KEY_Up: return KEY_UP;
        case XKB_KEY_Prior: return KEY_PAGE_UP;
        case XKB_KEY_Next: return KEY_PAGE_DOWN;
        case XKB_KEY_Home: return KEY_HOME;
        case XKB_KEY_End: return KEY_END;
        case XKB_KEY_Caps_Lock: return KEY_CAPS_LOCK;
        case XKB_KEY_Scroll_Lock: return KEY_SCROLL_LOCK;
        case XKB_KEY_Num_Lock: return KEY_NUM_LOCK;
        case XKB_KEY_Print: return KEY_PRINT_SCREEN;
        case XKB_KEY_Pause: return KEY_PAUSE;
        case XKB_KEY_Shift_L: return KEY_LEFT_SHIFT;
        case XKB_KEY_Control_L: return KEY_LEFT_CONTROL;
        case XKB_KEY_Alt_L: return KEY_LEFT_ALT;
        case XKB_KEY_Super_L: return KEY_LEFT_SUPER;
        case XKB_KEY_Shift_R: return KEY_RIGHT_SHIFT;
        case XKB_KEY_Control_R: return KEY_RIGHT_CONTROL;
        case XKB_KEY_Alt_R: case XKB_KEY_ISO_Level3_Shift: return KEY_RIGHT_ALT;
        case XKB_KEY_Super_R: return KEY_RIGHT_SUPER;
        case XKB_KEY_Menu: return KEY_KB_MENU;
        case XKB_KEY_KP_Decimal: case XKB_KEY_KP_Delete: return KEY_KP_DECIMAL;
        case XKB_KEY_KP_Divide: return KEY_KP_DIVIDE;
        case XKB_KEY_KP_Multiply: return KEY_KP_MULTIPLY;
        case XKB_KEY_KP_Subtract: return KEY_KP_SUBTRACT;
        case XKB_KEY_KP_Add: return KEY_KP_ADD;
        case XKB_KEY_KP_Enter: return KEY_KP_ENTER;
        case XKB_KEY_KP_Equal: return KEY_KP_EQUAL;
        case XKB_KEY_KP_Insert: return KEY_KP_0;
        case XKB_KEY_KP_End: return KEY_KP_1;
        case XKB_KEY_KP_Down: return KEY_KP_2;
        case XKB_KEY_KP_Next: return KEY_KP_3;
        case XKB_KEY_KP_Left: return KEY_KP_4;
        case XKB_KEY_KP_Begin: return KEY_KP_5;
        case XKB_KEY_KP_Right: return KEY_KP_6;
        case XKB_KEY_KP_Home: return KEY_KP_7;
        case XKB_KEY_KP_Up: return KEY_KP_8;
        case XKB_KEY_KP_Prior: return KEY_KP_9;
        default: break;
    }

    return KEY_NULL;
}

//----------------------------------------------------------------------------------
// Dropped files
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
        while ((start < length) && (uri[start] != '/')) start++;    // Optional hostname
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
static void StoreDroppedUriList(const char *uriList, size_t size)
{
    ClearDroppedFiles();

    CORE.window.dropFilepaths = (char **)RL_CALLOC(MAX_DROPPED_FILES, sizeof(char *));
    if (CORE.window.dropFilepaths == NULL) return;

    size_t i = 0;
    while ((i < size) && (CORE.window.dropFileCount < MAX_DROPPED_FILES))
    {
        while ((i < size) && ((uriList[i] == '\r') || (uriList[i] == '\n'))) i++;
        if (i >= size) break;

        size_t start = i;
        while ((i < size) && (uriList[i] != '\r') && (uriList[i] != '\n')) i++;

        int length = (int)(i - start);
        if ((length == 0) || (uriList[start] == '#')) continue;

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

//----------------------------------------------------------------------------------
// Data transfers (clipboard and drag and drop payloads)
//----------------------------------------------------------------------------------

// Read a whole offer payload through a pipe; returns an RL_MALLOC'd NUL-terminated
// buffer (caller frees) or NULL. *outSize gets the payload size without terminator.
static char *ReceiveOfferData(struct wl_data_offer *offer, const char *mime, size_t *outSize)
{
    if (outSize != NULL) *outSize = 0;
    if ((offer == NULL) || (platform.display == NULL)) return NULL;

    int fds[2] = { -1, -1 };
    if (pipe(fds) != 0) return NULL;

    wl_data_offer_receive(offer, mime, fds[1]);
    wl_display_flush(platform.display);
    close(fds[1]);

    char *data = NULL;
    size_t size = 0;

    for (;;)
    {
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, TRANSFER_TIMEOUT_MS);
        if (ready <= 0) break;              // Timeout or error: return what we have

        char chunk[4096];
        ssize_t got = read(fds[0], chunk, sizeof(chunk));
        if (got <= 0) break;                // EOF or error

        char *grown = (char *)RL_REALLOC(data, size + (size_t)got + 1);
        if (grown == NULL) break;
        data = grown;
        memcpy(data + size, chunk, (size_t)got);
        size += (size_t)got;
        data[size] = '\0';
    }

    close(fds[0]);

    if ((data != NULL) && (size == 0)) { RL_FREE(data); data = NULL; }
    if (outSize != NULL) *outSize = size;
    return data;
}

//----------------------------------------------------------------------------------
// wl_data_offer / wl_data_device / wl_data_source listeners
//----------------------------------------------------------------------------------

static void DataOfferHandleOffer(void *data, struct wl_data_offer *offer, const char *mimeType)
{
    (void)data;
    if ((mimeType == NULL) || (offer != platform.dragOffer)) return;
    if (strcmp(mimeType, MIME_URI_LIST) == 0) platform.dragOfferHasUriList = true;
}

static void DataOfferHandleSourceActions(void *data, struct wl_data_offer *offer, uint32_t actions)
{
    (void)data; (void)offer; (void)actions;
}

static void DataOfferHandleAction(void *data, struct wl_data_offer *offer, uint32_t action)
{
    (void)data; (void)offer; (void)action;
}

static const struct wl_data_offer_listener dataOfferListener = {
    .offer = DataOfferHandleOffer,
    .source_actions = DataOfferHandleSourceActions,
    .action = DataOfferHandleAction
};

static void DataDeviceHandleDataOffer(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)data; (void)device;
    // Offers are typed by the following enter/selection event; listen already to collect mimes
    platform.dragOffer = offer;
    platform.dragOfferHasUriList = false;
    wl_data_offer_add_listener(offer, &dataOfferListener, NULL);
}

static void DataDeviceHandleEnter(void *data, struct wl_data_device *device, uint32_t serial,
                                  struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                                  struct wl_data_offer *offer)
{
    (void)data; (void)device; (void)surface; (void)x; (void)y;
    if (offer == NULL) return;

    platform.dragOffer = offer;
    if (platform.dragOfferHasUriList)
    {
        wl_data_offer_accept(offer, serial, MIME_URI_LIST);
        if (wl_data_offer_get_version(offer) >= 3)
        {
            wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        }
    }
    else wl_data_offer_accept(offer, serial, NULL);
}

static void DataDeviceHandleLeave(void *data, struct wl_data_device *device)
{
    (void)data; (void)device;
    if (platform.dragOffer != NULL) wl_data_offer_destroy(platform.dragOffer);
    platform.dragOffer = NULL;
    platform.dragOfferHasUriList = false;
}

static void DataDeviceHandleMotion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)device; (void)time; (void)x; (void)y;
}

static void DataDeviceHandleDrop(void *data, struct wl_data_device *device)
{
    (void)data; (void)device;
    if ((platform.dragOffer == NULL) || !platform.dragOfferHasUriList)
    {
        if (platform.dragOffer != NULL) { wl_data_offer_destroy(platform.dragOffer); platform.dragOffer = NULL; }
        return;
    }

    size_t size = 0;
    char *payload = ReceiveOfferData(platform.dragOffer, MIME_URI_LIST, &size);
    if (payload != NULL)
    {
        StoreDroppedUriList(payload, size);
        RL_FREE(payload);
    }

    if (wl_data_offer_get_version(platform.dragOffer) >= 3) wl_data_offer_finish(platform.dragOffer);
    wl_data_offer_destroy(platform.dragOffer);
    platform.dragOffer = NULL;
    platform.dragOfferHasUriList = false;
}

static void DataDeviceHandleSelection(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)data; (void)device;

    if (platform.selectionOffer != NULL) wl_data_offer_destroy(platform.selectionOffer);
    platform.selectionOffer = offer;
    if (platform.dragOffer == offer) { platform.dragOffer = NULL; platform.dragOfferHasUriList = false; }
}

static const struct wl_data_device_listener dataDeviceListener = {
    .data_offer = DataDeviceHandleDataOffer,
    .enter = DataDeviceHandleEnter,
    .leave = DataDeviceHandleLeave,
    .motion = DataDeviceHandleMotion,
    .drop = DataDeviceHandleDrop,
    .selection = DataDeviceHandleSelection
};

static void DataSourceHandleTarget(void *data, struct wl_data_source *source, const char *mimeType)
{
    (void)data; (void)source; (void)mimeType;
}

static void DataSourceHandleSend(void *data, struct wl_data_source *source, const char *mimeType, int32_t fd)
{
    (void)data; (void)source; (void)mimeType;

    const char *text = platform.selectionOwnedText;
    if (text != NULL)
    {
        size_t remaining = strlen(text);
        while (remaining > 0)
        {
            ssize_t written = write(fd, text, remaining);
            if (written <= 0) break;
            text += written;
            remaining -= (size_t)written;
        }
    }

    close(fd);
}

static void DataSourceHandleCancelled(void *data, struct wl_data_source *source)
{
    (void)data;
    if (platform.selectionSource == source)
    {
        platform.selectionSource = NULL;
        RL_FREE(platform.selectionOwnedText);
        platform.selectionOwnedText = NULL;
    }
    wl_data_source_destroy(source);
}

static void DataSourceHandleDndDropPerformed(void *data, struct wl_data_source *source) { (void)data; (void)source; }
static void DataSourceHandleDndFinished(void *data, struct wl_data_source *source) { (void)data; (void)source; }
static void DataSourceHandleAction(void *data, struct wl_data_source *source, uint32_t action) { (void)data; (void)source; (void)action; }

static const struct wl_data_source_listener dataSourceListener = {
    .target = DataSourceHandleTarget,
    .send = DataSourceHandleSend,
    .cancelled = DataSourceHandleCancelled,
    .dnd_drop_performed = DataSourceHandleDndDropPerformed,
    .dnd_finished = DataSourceHandleDndFinished,
    .action = DataSourceHandleAction
};

//----------------------------------------------------------------------------------
// Pointer listeners
//----------------------------------------------------------------------------------

static void PointerHandleEnter(void *data, struct wl_pointer *pointer, uint32_t serial,
                               struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer;
    if (surface != platform.surface) return;

    platform.pointerEnterSerial = serial;
    platform.lastInputSerial = serial;
    CORE.input.mouse.cursorOnScreen = true;
    if (!CORE.input.mouse.cursorLocked)
    {
        CORE.input.mouse.currentPosition.x = (float)wl_fixed_to_double(x);
        CORE.input.mouse.currentPosition.y = (float)wl_fixed_to_double(y);
    }

    SetCursorShape(CORE.input.mouse.cursor);
}

static void PointerHandleLeave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)surface;
    platform.lastInputSerial = serial;
    CORE.input.mouse.cursorOnScreen = false;
}

static void PointerHandleMotion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer; (void)time;
    if (CORE.input.mouse.cursorLocked) return;      // Deltas arrive through the relative pointer

    CORE.input.mouse.currentPosition.x = (float)wl_fixed_to_double(x);
    CORE.input.mouse.currentPosition.y = (float)wl_fixed_to_double(y);
}

static void PointerHandleButton(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                                uint32_t button, uint32_t state)
{
    (void)data; (void)pointer; (void)time;
    platform.lastInputSerial = serial;

    int mouseButton = -1;
    switch (button)
    {
        case BTN_LEFT: mouseButton = MOUSE_BUTTON_LEFT; break;
        case BTN_RIGHT: mouseButton = MOUSE_BUTTON_RIGHT; break;
        case BTN_MIDDLE: mouseButton = MOUSE_BUTTON_MIDDLE; break;
        case BTN_SIDE: mouseButton = MOUSE_BUTTON_SIDE; break;
        case BTN_EXTRA: mouseButton = MOUSE_BUTTON_EXTRA; break;
        case BTN_FORWARD: mouseButton = MOUSE_BUTTON_FORWARD; break;
        case BTN_BACK: mouseButton = MOUSE_BUTTON_BACK; break;
        default: break;
    }

    if ((mouseButton < 0) || (mouseButton >= MAX_MOUSE_BUTTONS)) return;
    CORE.input.mouse.currentButtonState[mouseButton] = (state == WL_POINTER_BUTTON_STATE_PRESSED)? 1 : 0;
}

static void PointerHandleAxis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time;

    // Wayland reports scroll in surface-local units; normalize to raylib's ~1 per notch
    float amount = (float)(wl_fixed_to_double(value)/10.0);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) CORE.input.mouse.currentWheelMove.y -= amount;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) CORE.input.mouse.currentWheelMove.x += amount;
}

static void PointerHandleFrame(void *data, struct wl_pointer *pointer) { (void)data; (void)pointer; }
static void PointerHandleAxisSource(void *data, struct wl_pointer *pointer, uint32_t source) { (void)data; (void)pointer; (void)source; }
static void PointerHandleAxisStop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) { (void)data; (void)pointer; (void)time; (void)axis; }

static void PointerHandleAxisDiscrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer;
    // Discrete notches are authoritative when present: replace the accumulated smooth value
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) CORE.input.mouse.currentWheelMove.y = (float)-discrete;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) CORE.input.mouse.currentWheelMove.x = (float)discrete;
}

static const struct wl_pointer_listener pointerListener = {
    .enter = PointerHandleEnter,
    .leave = PointerHandleLeave,
    .motion = PointerHandleMotion,
    .button = PointerHandleButton,
    .axis = PointerHandleAxis,
    .frame = PointerHandleFrame,
    .axis_source = PointerHandleAxisSource,
    .axis_stop = PointerHandleAxisStop,
    .axis_discrete = PointerHandleAxisDiscrete
};

static void RelativePointerHandleMotion(void *data, struct zwp_relative_pointer_v1 *relativePointer,
                                        uint32_t timeHi, uint32_t timeLo,
                                        wl_fixed_t dx, wl_fixed_t dy,
                                        wl_fixed_t dxUnaccel, wl_fixed_t dyUnaccel)
{
    (void)data; (void)relativePointer; (void)timeHi; (void)timeLo; (void)dxUnaccel; (void)dyUnaccel;
    if (!CORE.input.mouse.cursorLocked) return;

    CORE.input.mouse.currentPosition.x += (float)wl_fixed_to_double(dx);
    CORE.input.mouse.currentPosition.y += (float)wl_fixed_to_double(dy);
}

static const struct zwp_relative_pointer_v1_listener relativePointerListener = {
    .relative_motion = RelativePointerHandleMotion
};

//----------------------------------------------------------------------------------
// Keyboard listeners
//----------------------------------------------------------------------------------

static void KeyboardHandleKeymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)keyboard;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }

    char *map = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(platform.xkbContext, map,
                                                           XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);

    if (keymap == NULL) { TRACELOG(LOG_WARNING, "PLATFORM: Failed to compile the compositor keymap"); return; }

    struct xkb_state *state = xkb_state_new(keymap);
    if (state == NULL) { xkb_keymap_unref(keymap); return; }

    if (platform.xkbState != NULL) xkb_state_unref(platform.xkbState);
    if (platform.xkbKeymap != NULL) xkb_keymap_unref(platform.xkbKeymap);
    platform.xkbKeymap = keymap;
    platform.xkbState = state;
}

static void KeyboardHandleEnter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                struct wl_surface *surface, struct wl_array *keys)
{
    (void)data; (void)keyboard; (void)surface; (void)keys;
    platform.lastInputSerial = serial;
    CORE.window.flags &= ~FLAG_WINDOW_UNFOCUSED;
}

static void KeyboardHandleLeave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)keyboard; (void)surface;
    platform.lastInputSerial = serial;
    CORE.window.flags |= FLAG_WINDOW_UNFOCUSED;

    // Nothing may stay logically pressed while the window has no keyboard focus
    memset(CORE.input.keyboard.currentKeyState, 0, sizeof(CORE.input.keyboard.currentKeyState));
    platform.repeatKey = 0;
}

static void KeyboardHandleKey(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                              uint32_t key, uint32_t state)
{
    (void)data; (void)keyboard; (void)time;
    platform.lastInputSerial = serial;
    if (platform.xkbState == NULL) return;

    xkb_keycode_t keycode = key + 8;    // evdev to xkb keycode offset
    xkb_keysym_t sym = xkb_state_key_get_one_sym(platform.xkbState, keycode);
    int raylibKey = TranslateKeysym(sym);

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
    {
        if ((raylibKey > 0) && (raylibKey < MAX_KEYBOARD_KEYS)) CORE.input.keyboard.currentKeyState[raylibKey] = 0;
        if (platform.repeatScancode == key) platform.repeatKey = 0;
        return;
    }

    QueueKeyPressed(raylibKey, false);

    char utf8[16] = { 0 };
    int length = xkb_state_key_get_utf8(platform.xkbState, keycode, utf8, sizeof(utf8));
    if (length > 0) PushCharsUtf8(utf8, (length < (int)sizeof(utf8))? length : (int)sizeof(utf8) - 1);

    if ((platform.repeatRate > 0) && (raylibKey > 0) && xkb_keymap_key_repeats(platform.xkbKeymap, keycode))
    {
        platform.repeatKey = raylibKey;
        platform.repeatScancode = key;
        platform.repeatNextTime = rlLinuxGetTime() + (double)platform.repeatDelay/1000.0;
    }
}

static void KeyboardHandleModifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                    uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data; (void)keyboard; (void)serial;
    if (platform.xkbState == NULL) return;
    xkb_state_update_mask(platform.xkbState, depressed, latched, locked, 0, 0, group);
}

static void KeyboardHandleRepeatInfo(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)data; (void)keyboard;
    platform.repeatRate = rate;
    platform.repeatDelay = delay;
}

static const struct wl_keyboard_listener keyboardListener = {
    .keymap = KeyboardHandleKeymap,
    .enter = KeyboardHandleEnter,
    .leave = KeyboardHandleLeave,
    .key = KeyboardHandleKey,
    .modifiers = KeyboardHandleModifiers,
    .repeat_info = KeyboardHandleRepeatInfo
};

//----------------------------------------------------------------------------------
// Seat / output / shell listeners
//----------------------------------------------------------------------------------

static void SeatHandleCapabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    (void)data;

    bool hasPointer = ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0);
    if (hasPointer && (platform.pointer == NULL))
    {
        platform.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(platform.pointer, &pointerListener, NULL);
    }
    else if (!hasPointer && (platform.pointer != NULL))
    {
        wl_pointer_release(platform.pointer);
        platform.pointer = NULL;
    }

    bool hasKeyboard = ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0);
    if (hasKeyboard && (platform.keyboard == NULL))
    {
        platform.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(platform.keyboard, &keyboardListener, NULL);
    }
    else if (!hasKeyboard && (platform.keyboard != NULL))
    {
        wl_keyboard_release(platform.keyboard);
        platform.keyboard = NULL;
    }
}

static void SeatHandleName(void *data, struct wl_seat *seat, const char *name) { (void)data; (void)seat; (void)name; }

static const struct wl_seat_listener seatListener = {
    .capabilities = SeatHandleCapabilities,
    .name = SeatHandleName
};

// Find the monitor record bound to a wl_output, NULL when unknown
static MonitorData *FindMonitor(struct wl_output *output)
{
    for (int i = 0; i < platform.monitorCount; i++)
    {
        if (platform.monitors[i].output == output) return &platform.monitors[i];
    }
    return NULL;
}

static void OutputHandleGeometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                                 int32_t physicalWidth, int32_t physicalHeight, int32_t subpixel,
                                 const char *make, const char *model, int32_t transform)
{
    (void)data; (void)subpixel; (void)transform;

    MonitorData *monitor = FindMonitor(output);
    if (monitor == NULL) return;

    monitor->x = x;
    monitor->y = y;
    monitor->physicalWidth = physicalWidth;
    monitor->physicalHeight = physicalHeight;

    if (monitor->name[0] == '\0')
    {
        const char *first = (make != NULL)? make : "";
        const char *second = (model != NULL)? model : "";
        snprintf(monitor->name, sizeof(monitor->name), "%s %s", first, second);
    }
}

static void OutputHandleMode(void *data, struct wl_output *output, uint32_t flags,
                             int32_t width, int32_t height, int32_t refresh)
{
    (void)data;
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) return;

    MonitorData *monitor = FindMonitor(output);
    if (monitor == NULL) return;

    monitor->width = width;
    monitor->height = height;
    monitor->refreshRate = (refresh + 500)/1000;    // mHz to Hz
}

static void OutputHandleDone(void *data, struct wl_output *output) { (void)data; (void)output; }

static void OutputHandleScale(void *data, struct wl_output *output, int32_t factor)
{
    (void)data;
    MonitorData *monitor = FindMonitor(output);
    if (monitor != NULL) monitor->scale = (factor > 0)? factor : 1;
}

static void OutputHandleName(void *data, struct wl_output *output, const char *name)
{
    (void)data;
    MonitorData *monitor = FindMonitor(output);
    if ((monitor == NULL) || (name == NULL)) return;

    strncpy(monitor->name, name, sizeof(monitor->name) - 1);
    monitor->name[sizeof(monitor->name) - 1] = '\0';
}

static void OutputHandleDescription(void *data, struct wl_output *output, const char *description)
{
    (void)data; (void)output; (void)description;
}

static const struct wl_output_listener outputListener = {
    .geometry = OutputHandleGeometry,
    .mode = OutputHandleMode,
    .done = OutputHandleDone,
    .scale = OutputHandleScale,
    .name = OutputHandleName,
    .description = OutputHandleDescription
};

static void SurfaceHandleEnter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data; (void)surface;
    platform.currentOutput = output;
}

static void SurfaceHandleLeave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data; (void)surface;
    if (platform.currentOutput == output) platform.currentOutput = NULL;
}

static const struct wl_surface_listener surfaceListener = {
    .enter = SurfaceHandleEnter,
    .leave = SurfaceHandleLeave
};

static void WmBaseHandlePing(void *data, struct xdg_wm_base *wmBase, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wmBase, serial);
}

static const struct xdg_wm_base_listener wmBaseListener = { .ping = WmBaseHandlePing };

static void XdgSurfaceHandleConfigure(void *data, struct xdg_surface *xdgSurface, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xdgSurface, serial);

    if (!platform.pendingConfigure) return;
    platform.pendingConfigure = false;

    int width = (platform.pendingWidth > 0)? platform.pendingWidth : CORE.window.screen.width;
    int height = (platform.pendingHeight > 0)? platform.pendingHeight : CORE.window.screen.height;
    UpdateWindowSize(width, height);

    CORE.window.fullscreen = platform.pendingFullscreen;
    if (platform.pendingFullscreen) CORE.window.flags |= FLAG_FULLSCREEN_MODE;
    else CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;
    if (platform.pendingMaximized) CORE.window.flags |= FLAG_WINDOW_MAXIMIZED;
    else CORE.window.flags &= ~FLAG_WINDOW_MAXIMIZED;
    if (platform.pendingActivated) CORE.window.flags &= ~FLAG_WINDOW_UNFOCUSED;
    else CORE.window.flags |= FLAG_WINDOW_UNFOCUSED;
}

static const struct xdg_surface_listener xdgSurfaceListener = { .configure = XdgSurfaceHandleConfigure };

static void ToplevelHandleConfigure(void *data, struct xdg_toplevel *toplevel,
                                    int32_t width, int32_t height, struct wl_array *states)
{
    (void)data; (void)toplevel;

    platform.pendingConfigure = true;
    platform.pendingWidth = width;
    platform.pendingHeight = height;
    platform.pendingFullscreen = false;
    platform.pendingMaximized = false;
    platform.pendingActivated = false;

    if (states == NULL) return;

    const uint32_t *state = NULL;
    for (state = (const uint32_t *)states->data;
         (const char *)state < ((const char *)states->data + states->size);
         state++)
    {
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN) platform.pendingFullscreen = true;
        else if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED) platform.pendingMaximized = true;
        else if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) platform.pendingActivated = true;
    }
}

static void ToplevelHandleClose(void *data, struct xdg_toplevel *toplevel)
{
    (void)data; (void)toplevel;
    CORE.window.shouldClose = true;
}

static const struct xdg_toplevel_listener toplevelListener = {
    .configure = ToplevelHandleConfigure,
    .close = ToplevelHandleClose
};

static void DecorationHandleConfigure(void *data, struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode)
{
    (void)data; (void)decoration;
    platform.serverSideDecorations = (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    if (!platform.serverSideDecorations && ((CORE.window.flags & FLAG_WINDOW_UNDECORATED) == 0))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Compositor requested client-side decorations, window stays undecorated");
    }
}

static const struct zxdg_toplevel_decoration_v1_listener decorationListener = { .configure = DecorationHandleConfigure };

static void LockedPointerHandleLocked(void *data, struct zwp_locked_pointer_v1 *lockedPointer) { (void)data; (void)lockedPointer; }
static void LockedPointerHandleUnlocked(void *data, struct zwp_locked_pointer_v1 *lockedPointer) { (void)data; (void)lockedPointer; }

static const struct zwp_locked_pointer_v1_listener lockedPointerListener = {
    .locked = LockedPointerHandleLocked,
    .unlocked = LockedPointerHandleUnlocked
};

//----------------------------------------------------------------------------------
// Registry
//----------------------------------------------------------------------------------

// Return the version to bind: the smaller of what the compositor offers and what we handle
static uint32_t BindVersion(uint32_t available, uint32_t wanted) { return (available < wanted)? available : wanted; }

static void RegistryHandleGlobal(void *data, struct wl_registry *registry, uint32_t name,
                                 const char *interface, uint32_t version)
{
    (void)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        platform.compositor = (struct wl_compositor *)wl_registry_bind(registry, name, &wl_compositor_interface, BindVersion(version, 4));
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        platform.shm = (struct wl_shm *)wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        platform.seat = (struct wl_seat *)wl_registry_bind(registry, name, &wl_seat_interface, BindVersion(version, 5));
        wl_seat_add_listener(platform.seat, &seatListener, NULL);
    }
    else if (strcmp(interface, wl_output_interface.name) == 0)
    {
        if (platform.monitorCount >= MAX_MONITORS) return;

        MonitorData *monitor = &platform.monitors[platform.monitorCount];
        memset(monitor, 0, sizeof(*monitor));
        monitor->scale = 1;
        monitor->output = (struct wl_output *)wl_registry_bind(registry, name, &wl_output_interface, BindVersion(version, 4));
        platform.monitorCount++;
        wl_output_add_listener(monitor->output, &outputListener, NULL);
    }
    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0)
    {
        platform.dataDeviceManager = (struct wl_data_device_manager *)wl_registry_bind(registry, name, &wl_data_device_manager_interface, BindVersion(version, 3));
    }
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
        platform.wmBase = (struct xdg_wm_base *)wl_registry_bind(registry, name, &xdg_wm_base_interface, BindVersion(version, 2));
        xdg_wm_base_add_listener(platform.wmBase, &wmBaseListener, NULL);
    }
    else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
    {
        platform.decorationManager = (struct zxdg_decoration_manager_v1 *)wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    }
    else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0)
    {
        platform.relativePointerManager = (struct zwp_relative_pointer_manager_v1 *)wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
    }
    else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0)
    {
        platform.pointerConstraints = (struct zwp_pointer_constraints_v1 *)wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    }
}

static void RegistryHandleGlobalRemove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
    // Output hot-unplug is rare and cannot invalidate our proxies mid-frame safely:
    // stale entries are filtered by their zero size when monitors are queried
}

static const struct wl_registry_listener registryListener = {
    .global = RegistryHandleGlobal,
    .global_remove = RegistryHandleGlobalRemove
};

//----------------------------------------------------------------------------------
// Cursor
//----------------------------------------------------------------------------------

// Map a raylib MouseCursor to the XDG/CSS cursor name used by the cursor theme
static const char *CursorThemeName(int cursor)
{
    switch (cursor)
    {
        case MOUSE_CURSOR_ARROW: case MOUSE_CURSOR_DEFAULT: return "left_ptr";
        case MOUSE_CURSOR_IBEAM: return "xterm";
        case MOUSE_CURSOR_CROSSHAIR: return "crosshair";
        case MOUSE_CURSOR_POINTING_HAND: return "hand2";
        case MOUSE_CURSOR_RESIZE_EW: return "sb_h_double_arrow";
        case MOUSE_CURSOR_RESIZE_NS: return "sb_v_double_arrow";
        case MOUSE_CURSOR_RESIZE_NWSE: return "bottom_right_corner";
        case MOUSE_CURSOR_RESIZE_NESW: return "bottom_left_corner";
        case MOUSE_CURSOR_RESIZE_ALL: return "fleur";
        case MOUSE_CURSOR_NOT_ALLOWED: return "crossed_circle";
        default: break;
    }
    return "left_ptr";
}

// Attach the themed cursor image to the pointer, or hide it when requested
static void SetCursorShape(int cursor)
{
    if (platform.pointer == NULL) return;

    if (CORE.input.mouse.cursorHidden || CORE.input.mouse.cursorLocked)
    {
        wl_pointer_set_cursor(platform.pointer, platform.pointerEnterSerial, NULL, 0, 0);
        return;
    }

    if ((platform.cursorTheme == NULL) || (platform.cursorSurface == NULL)) return;

    struct wl_cursor *shape = wl_cursor_theme_get_cursor(platform.cursorTheme, CursorThemeName(cursor));
    if (shape == NULL) shape = wl_cursor_theme_get_cursor(platform.cursorTheme, "left_ptr");
    if ((shape == NULL) || (shape->image_count == 0)) return;

    struct wl_cursor_image *image = shape->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (buffer == NULL) return;

    wl_pointer_set_cursor(platform.pointer, platform.pointerEnterSerial, platform.cursorSurface,
                          (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
    wl_surface_attach(platform.cursorSurface, buffer, 0, 0);
    wl_surface_damage(platform.cursorSurface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(platform.cursorSurface);
}

//----------------------------------------------------------------------------------
// Vulkan surface
//----------------------------------------------------------------------------------

// rlvk surface callback: create the VK_KHR_wayland_surface for our wl_surface
static uint64_t CreateSurfaceWayland(void *instance, void *userData)
{
    (void)userData;

    PFN_vkCreateWaylandSurfaceKHR createWaylandSurface =
        (PFN_vkCreateWaylandSurfaceKHR)vkGetInstanceProcAddr((VkInstance)instance, "vkCreateWaylandSurfaceKHR");
    if (createWaylandSurface == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: vkCreateWaylandSurfaceKHR not available in the Vulkan instance");
        return 0;
    }

    VkWaylandSurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .pNext = NULL,
        .flags = 0,
        .display = platform.display,
        .surface = platform.surface
    };

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (createWaylandSurface((VkInstance)instance, &info, NULL, &surface) != VK_SUCCESS)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create Vulkan Wayland surface");
        return 0;
    }

    return (uint64_t)surface;
}

//----------------------------------------------------------------------------------
// Platform contract: initialization
//----------------------------------------------------------------------------------

// Scale factor of the output the window currently sits on (1 when HighDPI is off)
static int GetPreferredScale(void)
{
    if ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) == 0) return 1;

    MonitorData *monitor = (platform.currentOutput != NULL)? FindMonitor(platform.currentOutput) : NULL;
    if ((monitor == NULL) && (platform.monitorCount > 0)) monitor = &platform.monitors[0];
    if (monitor == NULL) return 1;

    return (monitor->scale > 0)? monitor->scale : 1;
}

int InitPlatform(void)
{
    platform.display = wl_display_connect(NULL);
    if (platform.display == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to connect to the Wayland display");
        return -1;
    }

    platform.registry = wl_display_get_registry(platform.display);
    wl_registry_add_listener(platform.registry, &registryListener, NULL);
    wl_display_roundtrip(platform.display);     // Globals
    wl_display_roundtrip(platform.display);     // Their initial events (seat caps, output modes)

    if ((platform.compositor == NULL) || (platform.wmBase == NULL))
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Compositor is missing wl_compositor or xdg_wm_base");
        ClosePlatform();
        return -1;
    }

    platform.xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (platform.xkbContext == NULL) TRACELOG(LOG_WARNING, "PLATFORM: Failed to create the xkb context, keyboard input disabled");

    if (CORE.window.screen.width <= 0) CORE.window.screen.width = 800;
    if (CORE.window.screen.height <= 0) CORE.window.screen.height = 450;

    // Display size: the first advertised output is the reference, as raylib expects one
    if (platform.monitorCount > 0)
    {
        CORE.window.display.width = platform.monitors[0].width;
        CORE.window.display.height = platform.monitors[0].height;
    }

    platform.surface = wl_compositor_create_surface(platform.compositor);
    if (platform.surface == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create the Wayland surface");
        ClosePlatform();
        return -1;
    }
    wl_surface_add_listener(platform.surface, &surfaceListener, NULL);

    platform.xdgSurface = xdg_wm_base_get_xdg_surface(platform.wmBase, platform.surface);
    xdg_surface_add_listener(platform.xdgSurface, &xdgSurfaceListener, NULL);
    platform.xdgToplevel = xdg_surface_get_toplevel(platform.xdgSurface);
    xdg_toplevel_add_listener(platform.xdgToplevel, &toplevelListener, NULL);

    const char *title = (CORE.window.title != NULL)? CORE.window.title : "rayvulkan";
    xdg_toplevel_set_title(platform.xdgToplevel, title);
    xdg_toplevel_set_app_id(platform.xdgToplevel, title);

    if (platform.decorationManager != NULL)
    {
        platform.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(platform.decorationManager, platform.xdgToplevel);
        zxdg_toplevel_decoration_v1_add_listener(platform.decoration, &decorationListener, NULL);
        zxdg_toplevel_decoration_v1_set_mode(platform.decoration,
            ((CORE.window.flags & FLAG_WINDOW_UNDECORATED) != 0)?
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    else if ((CORE.window.flags & FLAG_WINDOW_UNDECORATED) == 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: No xdg-decoration support, the window will have no decorations");
    }

    if ((CORE.window.flags & FLAG_WINDOW_RESIZABLE) == 0)
    {
        xdg_toplevel_set_min_size(platform.xdgToplevel, CORE.window.screen.width, CORE.window.screen.height);
        xdg_toplevel_set_max_size(platform.xdgToplevel, CORE.window.screen.width, CORE.window.screen.height);
    }
    if ((CORE.window.flags & FLAG_FULLSCREEN_MODE) != 0) xdg_toplevel_set_fullscreen(platform.xdgToplevel, NULL);
    if ((CORE.window.flags & FLAG_WINDOW_MAXIMIZED) != 0) xdg_toplevel_set_maximized(platform.xdgToplevel);
    if ((CORE.window.flags & FLAG_WINDOW_MINIMIZED) != 0) xdg_toplevel_set_minimized(platform.xdgToplevel);
    if ((CORE.window.flags & (FLAG_WINDOW_TOPMOST | FLAG_WINDOW_MOUSE_PASSTHROUGH | FLAG_INTERLACED_HINT)) != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Topmost/mouse-passthrough/interlaced flags are not supported on Wayland");
    }

    wl_surface_commit(platform.surface);
    wl_display_roundtrip(platform.display);     // Wait for the first configure

    if (platform.dataDeviceManager != NULL)
    {
        if (platform.seat != NULL)
        {
            platform.dataDevice = wl_data_device_manager_get_data_device(platform.dataDeviceManager, platform.seat);
            wl_data_device_add_listener(platform.dataDevice, &dataDeviceListener, NULL);
        }
    }
    else TRACELOG(LOG_WARNING, "PLATFORM: No wl_data_device_manager, clipboard and file drop are unavailable");

    int scale = GetPreferredScale();
    CORE.window.scale.x = (float)scale;
    CORE.window.scale.y = (float)scale;
    if (scale > 1) wl_surface_set_buffer_scale(platform.surface, scale);

    CORE.window.render.width = CORE.window.screen.width*scale;
    CORE.window.render.height = CORE.window.screen.height*scale;
    CORE.window.position.x = 0;     // Wayland clients cannot know their absolute position
    CORE.window.position.y = 0;
    CORE.window.previousScreen.width = CORE.window.screen.width;
    CORE.window.previousScreen.height = CORE.window.screen.height;

    // Cursor theme (size follows the buffer scale so the pointer is not blurry)
    platform.cursorThemeSize = 24*scale;
    if (platform.shm != NULL)
    {
        platform.cursorTheme = wl_cursor_theme_load(NULL, platform.cursorThemeSize, platform.shm);
        if (platform.cursorTheme == NULL) TRACELOG(LOG_WARNING, "PLATFORM: Failed to load a cursor theme");
    }
    if (platform.compositor != NULL) platform.cursorSurface = wl_compositor_create_surface(platform.compositor);

    static const char *extensions[2] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    rlvkPlatformGlue glue = {
        .requiredExtensions = extensions,
        .requiredExtensionCount = 2,
        .createSurface = CreateSurfaceWayland,
        .userData = NULL
    };

    if (!rlvkInit(&glue, CORE.window.render.width, CORE.window.render.height, (CORE.window.flags & FLAG_VSYNC_HINT) != 0))
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to initialize Vulkan rendering backend");
        ClosePlatform();
        return -1;
    }

    rlLinuxInitGamepads();

    platform.ready = true;
    CORE.window.ready = true;
    TRACELOG(LOG_INFO, "PLATFORM: Initialized successfully (Wayland)");
    return 0;
}

void ClosePlatform(void)
{
    rlLinuxCloseGamepads();

    if (platform.display == NULL) return;

    ClearDroppedFiles();
    RL_FREE(platform.selectionOwnedText);
    platform.selectionOwnedText = NULL;
    RL_FREE(platform.clipboardFetched);
    platform.clipboardFetched = NULL;

    if (platform.selectionSource != NULL) { wl_data_source_destroy(platform.selectionSource); platform.selectionSource = NULL; }
    if (platform.selectionOffer != NULL) { wl_data_offer_destroy(platform.selectionOffer); platform.selectionOffer = NULL; }
    if (platform.dragOffer != NULL) { wl_data_offer_destroy(platform.dragOffer); platform.dragOffer = NULL; }
    if (platform.dataDevice != NULL) { wl_data_device_destroy(platform.dataDevice); platform.dataDevice = NULL; }

    if (platform.lockedPointer != NULL) { zwp_locked_pointer_v1_destroy(platform.lockedPointer); platform.lockedPointer = NULL; }
    if (platform.relativePointer != NULL) { zwp_relative_pointer_v1_destroy(platform.relativePointer); platform.relativePointer = NULL; }

    if (platform.cursorSurface != NULL) { wl_surface_destroy(platform.cursorSurface); platform.cursorSurface = NULL; }
    if (platform.cursorTheme != NULL) { wl_cursor_theme_destroy(platform.cursorTheme); platform.cursorTheme = NULL; }

    if (platform.xkbState != NULL) { xkb_state_unref(platform.xkbState); platform.xkbState = NULL; }
    if (platform.xkbKeymap != NULL) { xkb_keymap_unref(platform.xkbKeymap); platform.xkbKeymap = NULL; }
    if (platform.xkbContext != NULL) { xkb_context_unref(platform.xkbContext); platform.xkbContext = NULL; }

    if (platform.keyboard != NULL) { wl_keyboard_release(platform.keyboard); platform.keyboard = NULL; }
    if (platform.pointer != NULL) { wl_pointer_release(platform.pointer); platform.pointer = NULL; }

    if (platform.decoration != NULL) { zxdg_toplevel_decoration_v1_destroy(platform.decoration); platform.decoration = NULL; }
    if (platform.xdgToplevel != NULL) { xdg_toplevel_destroy(platform.xdgToplevel); platform.xdgToplevel = NULL; }
    if (platform.xdgSurface != NULL) { xdg_surface_destroy(platform.xdgSurface); platform.xdgSurface = NULL; }
    if (platform.surface != NULL) { wl_surface_destroy(platform.surface); platform.surface = NULL; }

    if (platform.registry != NULL) { wl_registry_destroy(platform.registry); platform.registry = NULL; }

    wl_display_flush(platform.display);
    wl_display_disconnect(platform.display);
    platform.display = NULL;

    platform.ready = false;
    CORE.window.ready = false;
}

//----------------------------------------------------------------------------------
// Platform contract: frame loop
//----------------------------------------------------------------------------------

// Pump the Wayland event queue without blocking; safe against the prepare_read protocol
static void DispatchPending(void)
{
    while (wl_display_prepare_read(platform.display) != 0) wl_display_dispatch_pending(platform.display);
    wl_display_flush(platform.display);

    struct pollfd pfd = { .fd = wl_display_get_fd(platform.display), .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 0) > 0) wl_display_read_events(platform.display);
    else wl_display_cancel_read(platform.display);

    wl_display_dispatch_pending(platform.display);
}

// Re-emit the repeating key according to the compositor repeat rate/delay
static void UpdateKeyRepeat(void)
{
    if ((platform.repeatKey == 0) || (platform.repeatRate <= 0)) return;

    double now = rlLinuxGetTime();
    double interval = 1.0/(double)platform.repeatRate;
    while (now >= platform.repeatNextTime)
    {
        QueueKeyPressed(platform.repeatKey, true);
        platform.repeatNextTime += interval;
    }
}

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

    if (CORE.window.eventWaiting)
    {
        wl_display_flush(platform.display);
        if (wl_display_dispatch(platform.display) < 0) CORE.window.shouldClose = true;    // Compositor gone
    }

    DispatchPending();
    UpdateKeyRepeat();

    if (wl_display_get_error(platform.display) != 0)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Wayland connection error, requesting close");
        CORE.window.shouldClose = true;
    }
}

double GetTimePlatform(void) { return rlLinuxGetTime(); }
void WaitTimePlatform(double seconds) { rlLinuxWaitTime(seconds); }

void SwapScreenBufferPlatform(void)
{
    rlvkEndFrame();
    if (platform.display != NULL) wl_display_flush(platform.display);
}

//----------------------------------------------------------------------------------
// Platform contract: window control
//----------------------------------------------------------------------------------

void SetWindowTitlePlatform(const char *title)
{
    if (!WindowValid() || (title == NULL)) return;
    xdg_toplevel_set_title(platform.xdgToplevel, title);
    wl_display_flush(platform.display);
}

void SetWindowPositionPlatform(int x, int y)
{
    (void)x; (void)y;
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowPosition() is not supported on Wayland");
}

void SetWindowSizePlatform(int width, int height)
{
    if (!WindowValid() || (width <= 0) || (height <= 0)) return;

    // A client cannot resize itself: apply locally and let the compositor confirm
    if ((CORE.window.flags & FLAG_WINDOW_RESIZABLE) == 0)
    {
        xdg_toplevel_set_min_size(platform.xdgToplevel, width, height);
        xdg_toplevel_set_max_size(platform.xdgToplevel, width, height);
    }

    UpdateWindowSize(width, height);
    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void SetWindowMinSizePlatform(int width, int height)
{
    CORE.window.screenMin.width = (width > 0)? width : 0;
    CORE.window.screenMin.height = (height > 0)? height : 0;
    if (!WindowValid()) return;

    xdg_toplevel_set_min_size(platform.xdgToplevel, CORE.window.screenMin.width, CORE.window.screenMin.height);
    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void SetWindowMaxSizePlatform(int width, int height)
{
    CORE.window.screenMax.width = (width > 0)? width : 0;
    CORE.window.screenMax.height = (height > 0)? height : 0;
    if (!WindowValid()) return;

    xdg_toplevel_set_max_size(platform.xdgToplevel, CORE.window.screenMax.width, CORE.window.screenMax.height);
    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void SetWindowOpacityPlatform(float opacity)
{
    (void)opacity;
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowOpacity() is not supported on Wayland");
}

void SetWindowIconPlatform(Image *images, int count)
{
    (void)images; (void)count;
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowIcon() is not supported on Wayland, the icon comes from the .desktop app id");
}

void SetWindowFocusedPlatform(void)
{
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowFocused() is not supported on Wayland");
}

void SetWindowMonitorPlatform(int monitor)
{
    if (!WindowValid()) return;
    if ((monitor < 0) || (monitor >= platform.monitorCount))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return;
    }

    // Only fullscreen placement can be requested explicitly on Wayland
    if (CORE.window.fullscreen) xdg_toplevel_set_fullscreen(platform.xdgToplevel, platform.monitors[monitor].output);
    else TRACELOG(LOG_WARNING, "PLATFORM: SetWindowMonitor() only applies to fullscreen windows on Wayland");

    wl_display_flush(platform.display);
}

void SetWindowStatePlatform(unsigned int flags)
{
    if (!WindowValid()) return;

    if ((flags & FLAG_VSYNC_HINT) != 0) rlvkSetVSync(true);
    if ((flags & FLAG_FULLSCREEN_MODE) != 0) xdg_toplevel_set_fullscreen(platform.xdgToplevel, NULL);
    if ((flags & FLAG_WINDOW_MAXIMIZED) != 0) xdg_toplevel_set_maximized(platform.xdgToplevel);
    if ((flags & FLAG_WINDOW_MINIMIZED) != 0) xdg_toplevel_set_minimized(platform.xdgToplevel);
    if ((flags & FLAG_WINDOW_RESIZABLE) != 0)
    {
        xdg_toplevel_set_min_size(platform.xdgToplevel, CORE.window.screenMin.width, CORE.window.screenMin.height);
        xdg_toplevel_set_max_size(platform.xdgToplevel, CORE.window.screenMax.width, CORE.window.screenMax.height);
    }
    if (((flags & FLAG_WINDOW_UNDECORATED) != 0) && (platform.decoration != NULL))
    {
        zxdg_toplevel_decoration_v1_set_mode(platform.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }
    if ((flags & (FLAG_WINDOW_TOPMOST | FLAG_WINDOW_MOUSE_PASSTHROUGH | FLAG_WINDOW_HIDDEN |
                  FLAG_WINDOW_TRANSPARENT | FLAG_INTERLACED_HINT | FLAG_MSAA_4X_HINT)) != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Some requested window flags are not supported on Wayland");
    }

    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void ClearWindowStatePlatform(unsigned int flags)
{
    if (!WindowValid()) return;

    if ((flags & FLAG_VSYNC_HINT) != 0) rlvkSetVSync(false);
    if ((flags & FLAG_FULLSCREEN_MODE) != 0) xdg_toplevel_unset_fullscreen(platform.xdgToplevel);
    if ((flags & FLAG_WINDOW_MAXIMIZED) != 0) xdg_toplevel_unset_maximized(platform.xdgToplevel);
    if ((flags & FLAG_WINDOW_RESIZABLE) != 0)
    {
        xdg_toplevel_set_min_size(platform.xdgToplevel, CORE.window.screen.width, CORE.window.screen.height);
        xdg_toplevel_set_max_size(platform.xdgToplevel, CORE.window.screen.width, CORE.window.screen.height);
    }
    if (((flags & FLAG_WINDOW_UNDECORATED) != 0) && (platform.decoration != NULL))
    {
        zxdg_toplevel_decoration_v1_set_mode(platform.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void ToggleFullscreenPlatform(void)
{
    if (!WindowValid()) return;

    if (!CORE.window.fullscreen)
    {
        CORE.window.previousScreen.width = CORE.window.screen.width;
        CORE.window.previousScreen.height = CORE.window.screen.height;
        xdg_toplevel_set_fullscreen(platform.xdgToplevel, NULL);
    }
    else xdg_toplevel_unset_fullscreen(platform.xdgToplevel);

    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void ToggleBorderlessWindowedPlatform(void)
{
    if (!WindowValid()) return;

    // Wayland has no borderless-windowed concept: fullscreen without decorations is the
    // closest equivalent, so mirror the fullscreen request and drop the decorations
    if ((CORE.window.flags & FLAG_BORDERLESS_WINDOWED_MODE) == 0)
    {
        CORE.window.flags |= FLAG_BORDERLESS_WINDOWED_MODE;
        if (platform.decoration != NULL) zxdg_toplevel_decoration_v1_set_mode(platform.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        xdg_toplevel_set_fullscreen(platform.xdgToplevel, NULL);
    }
    else
    {
        CORE.window.flags &= ~FLAG_BORDERLESS_WINDOWED_MODE;
        xdg_toplevel_unset_fullscreen(platform.xdgToplevel);
        if (platform.decoration != NULL) zxdg_toplevel_decoration_v1_set_mode(platform.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void MaximizeWindowPlatform(void)
{
    if (!WindowValid()) return;
    xdg_toplevel_set_maximized(platform.xdgToplevel);
    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void MinimizeWindowPlatform(void)
{
    if (!WindowValid()) return;
    xdg_toplevel_set_minimized(platform.xdgToplevel);
    CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
    wl_display_flush(platform.display);
}

void RestoreWindowPlatform(void)
{
    if (!WindowValid()) return;
    xdg_toplevel_unset_maximized(platform.xdgToplevel);
    CORE.window.flags &= ~(FLAG_WINDOW_MAXIMIZED | FLAG_WINDOW_MINIMIZED);
    wl_surface_commit(platform.surface);
    wl_display_flush(platform.display);
}

void *GetWindowHandlePlatform(void) { return (void *)platform.surface; }

//----------------------------------------------------------------------------------
// Platform contract: monitors
//----------------------------------------------------------------------------------

int GetMonitorCountPlatform(void) { return platform.monitorCount; }

int GetCurrentMonitorPlatform(void)
{
    for (int i = 0; i < platform.monitorCount; i++)
    {
        if (platform.monitors[i].output == platform.currentOutput) return i;
    }
    return 0;
}

// Query one monitor safely; returns NULL and warns when the index is invalid
static const MonitorData *GetMonitorChecked(int monitor)
{
    if ((monitor < 0) || (monitor >= platform.monitorCount))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return NULL;
    }
    return &platform.monitors[monitor];
}

Vector2 GetMonitorPositionPlatform(int monitor)
{
    const MonitorData *info = GetMonitorChecked(monitor);
    if (info == NULL) return (Vector2){ 0.0f, 0.0f };
    return (Vector2){ (float)info->x, (float)info->y };
}

int GetMonitorWidthPlatform(int monitor)
{
    const MonitorData *info = GetMonitorChecked(monitor);
    return (info != NULL)? info->width : 0;
}

int GetMonitorHeightPlatform(int monitor)
{
    const MonitorData *info = GetMonitorChecked(monitor);
    return (info != NULL)? info->height : 0;
}

int GetMonitorPhysicalWidthPlatform(int monitor)
{
    const MonitorData *info = GetMonitorChecked(monitor);
    return (info != NULL)? info->physicalWidth : 0;
}

int GetMonitorPhysicalHeightPlatform(int monitor)
{
    const MonitorData *info = GetMonitorChecked(monitor);
    return (info != NULL)? info->physicalHeight : 0;
}

int GetMonitorRefreshRatePlatform(int monitor)
{
    const MonitorData *info = GetMonitorChecked(monitor);
    return (info != NULL)? info->refreshRate : 0;
}

const char *GetMonitorNamePlatform(int monitor)
{
    static char name[64] = { 0 };

    const MonitorData *info = GetMonitorChecked(monitor);
    if (info == NULL) { name[0] = '\0'; return name; }

    strncpy(name, info->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    return name;
}

Vector2 GetWindowScaleDPIPlatform(void) { return (Vector2){ CORE.window.scale.x, CORE.window.scale.y }; }

//----------------------------------------------------------------------------------
// Platform contract: cursor and clipboard
//----------------------------------------------------------------------------------

void ShowCursorPlatform(void)
{
    CORE.input.mouse.cursorHidden = false;
    SetCursorShape(CORE.input.mouse.cursor);
}

void HideCursorPlatform(void)
{
    CORE.input.mouse.cursorHidden = true;
    SetCursorShape(CORE.input.mouse.cursor);
}

void EnableCursorPlatform(void)
{
    if (platform.lockedPointer != NULL) { zwp_locked_pointer_v1_destroy(platform.lockedPointer); platform.lockedPointer = NULL; }
    if (platform.relativePointer != NULL) { zwp_relative_pointer_v1_destroy(platform.relativePointer); platform.relativePointer = NULL; }

    CORE.input.mouse.cursorLocked = false;
    CORE.input.mouse.cursorHidden = false;
    SetCursorShape(CORE.input.mouse.cursor);
    if (platform.display != NULL) wl_display_flush(platform.display);
}

void DisableCursorPlatform(void)
{
    if ((platform.pointer == NULL) || (platform.surface == NULL)) return;

    if ((platform.pointerConstraints == NULL) || (platform.relativePointerManager == NULL))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Compositor lacks pointer-constraints/relative-pointer, cursor lock unavailable");
        CORE.input.mouse.cursorHidden = true;
        SetCursorShape(CORE.input.mouse.cursor);
        return;
    }

    if (platform.lockedPointer == NULL)
    {
        platform.lockedPointer = zwp_pointer_constraints_v1_lock_pointer(platform.pointerConstraints, platform.surface,
                                                                        platform.pointer, NULL,
                                                                        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        if (platform.lockedPointer != NULL) zwp_locked_pointer_v1_add_listener(platform.lockedPointer, &lockedPointerListener, NULL);
    }

    if (platform.relativePointer == NULL)
    {
        platform.relativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer(platform.relativePointerManager, platform.pointer);
        if (platform.relativePointer != NULL) zwp_relative_pointer_v1_add_listener(platform.relativePointer, &relativePointerListener, NULL);
    }

    CORE.input.mouse.cursorLocked = true;
    CORE.input.mouse.cursorHidden = true;
    SetCursorShape(CORE.input.mouse.cursor);
    wl_display_flush(platform.display);
}

void SetMouseCursorPlatform(int cursor)
{
    if ((cursor < 0) || (cursor > MOUSE_CURSOR_NOT_ALLOWED))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Unsupported mouse cursor shape (%i)", cursor);
        return;
    }

    CORE.input.mouse.cursor = cursor;
    SetCursorShape(cursor);
}

void SetMousePositionPlatform(int x, int y)
{
    CORE.input.mouse.currentPosition.x = (float)x;
    CORE.input.mouse.currentPosition.y = (float)y;
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;

    if (platform.lockedPointer != NULL)
    {
        zwp_locked_pointer_v1_set_cursor_position_hint(platform.lockedPointer, wl_fixed_from_int(x), wl_fixed_from_int(y));
        wl_surface_commit(platform.surface);
        wl_display_flush(platform.display);
        return;
    }

    TRACELOG(LOG_WARNING, "PLATFORM: SetMousePosition() only moves the logical cursor on Wayland");
}

void SetClipboardTextPlatform(const char *text)
{
    if ((text == NULL) || (platform.dataDeviceManager == NULL) || (platform.dataDevice == NULL)) return;

    RL_FREE(platform.selectionOwnedText);
    size_t length = strlen(text);
    platform.selectionOwnedText = (char *)RL_MALLOC(length + 1);
    if (platform.selectionOwnedText == NULL) return;
    memcpy(platform.selectionOwnedText, text, length + 1);

    if (platform.selectionSource != NULL) wl_data_source_destroy(platform.selectionSource);
    platform.selectionSource = wl_data_device_manager_create_data_source(platform.dataDeviceManager);
    if (platform.selectionSource == NULL) return;

    wl_data_source_add_listener(platform.selectionSource, &dataSourceListener, NULL);
    wl_data_source_offer(platform.selectionSource, MIME_TEXT);
    wl_data_source_offer(platform.selectionSource, "text/plain");
    wl_data_source_offer(platform.selectionSource, "TEXT");
    wl_data_source_offer(platform.selectionSource, "STRING");
    wl_data_source_offer(platform.selectionSource, "UTF8_STRING");

    wl_data_device_set_selection(platform.dataDevice, platform.selectionSource, platform.lastInputSerial);
    wl_display_flush(platform.display);
}

const char *GetClipboardTextPlatform(void)
{
    // We own the selection: no round trip through the compositor needed
    if (platform.selectionSource != NULL) return platform.selectionOwnedText;
    if (platform.selectionOffer == NULL) return NULL;

    size_t size = 0;
    char *data = ReceiveOfferData(platform.selectionOffer, MIME_TEXT, &size);
    if (data == NULL) data = ReceiveOfferData(platform.selectionOffer, "text/plain", &size);
    if (data == NULL) return NULL;

    RL_FREE(platform.clipboardFetched);
    platform.clipboardFetched = data;
    return platform.clipboardFetched;
}

Image GetClipboardImagePlatform(void)
{
    Image image = { 0 };
    if (platform.selectionOffer == NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: No image/png data available in the clipboard");
        return image;
    }

    size_t size = 0;
    char *data = ReceiveOfferData(platform.selectionOffer, MIME_IMAGE_PNG, &size);
    if ((data == NULL) || (size == 0))
    {
        RL_FREE(data);
        TRACELOG(LOG_WARNING, "PLATFORM: No image/png data available in the clipboard");
        return image;
    }

    image = LoadImageFromMemory(".png", (const unsigned char *)data, (int)size);
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
        int status = 0;
        waitpid(pid, &status, WNOHANG);
    }
}

void SetGamepadVibrationPlatform(int gamepad, float leftMotor, float rightMotor, float duration)
{
    rlLinuxSetGamepadVibration(gamepad, leftMotor, rightMotor, duration);
}

int SetGamepadMappingsPlatform(const char *mappings) { return rlLinuxSetGamepadMappings(mappings); }
