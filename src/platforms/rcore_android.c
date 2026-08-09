/**********************************************************************************************
*
*   rcore_android - raylib-on-vulkan platform backend for Android (self contained NativeActivity glue)
*
*   This backend does NOT use android_native_app_glue.h; the glue is implemented here.
*
*   THREADING MODEL (the classic two-thread glue, chosen deliberately)
*     - The Android framework calls ANativeActivity_onCreate() and all activity callbacks
*       on the process UI thread. Blocking that thread stalls the whole app, and raylib's
*       API is a blocking main loop, so the game cannot run there.
*     - ANativeActivity_onCreate() therefore starts a second thread which calls the user's
*       main(). Everything raylib touches (CORE, Vulkan, the event pump) lives on that
*       thread only.
*     - UI thread callbacks push a one byte command through a pipe and, for the callbacks
*       that must not return before the game thread has acted (window created/destroyed,
*       input queue changes, save state, destroy), block on a condition variable until the
*       game thread acknowledges. This is the same contract the official glue implements.
*
*   ENTRY POINT
*     The library exports ANativeActivity_onCreate(). The application only provides a
*     normal main(); it is called by the glue on the game thread once the activity exists,
*     mirroring what raylib does with its android_main trampoline. android_main() is also
*     exported as a weak-compatible alias entry point for existing NDK build scripts.
*
*   NOT SUPPORTED (documented, never fatal)
*     - Clipboard images: Android exposes no image clipboard; an empty Image + warning.
*     - Mouse cursor shapes / warping: no system cursor on touch devices.
*     - Window icon, opacity, position, multiple monitors: no such concepts.
*     - JNI failures (clipboard text, OpenURL) log a warning and return safely.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#include "rcore_internal.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>

#include <jni.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
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
// Glue types
//----------------------------------------------------------------------------------
#define LOOPER_ID_MAIN      1
#define LOOPER_ID_INPUT     2

// Commands travelling from the UI thread to the game thread through the pipe
enum {
    APP_CMD_INPUT_CHANGED = 1,
    APP_CMD_INIT_WINDOW,
    APP_CMD_TERM_WINDOW,
    APP_CMD_WINDOW_RESIZED,
    APP_CMD_WINDOW_REDRAW,
    APP_CMD_GAINED_FOCUS,
    APP_CMD_LOST_FOCUS,
    APP_CMD_CONFIG_CHANGED,
    APP_CMD_LOW_MEMORY,
    APP_CMD_START,
    APP_CMD_RESUME,
    APP_CMD_SAVE_STATE,
    APP_CMD_PAUSE,
    APP_CMD_STOP,
    APP_CMD_DESTROY
};

typedef struct AndroidApp {
    ANativeActivity *activity;
    AConfiguration *config;
    ALooper *looper;

    ANativeWindow *window;              // Owned by the game thread
    AInputQueue *inputQueue;            // Owned by the game thread

    ANativeWindow *pendingWindow;       // Written by the UI thread
    AInputQueue *pendingInputQueue;     // Written by the UI thread

    void *savedState;
    size_t savedStateSize;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;

    int readPipe;
    int writePipe;

    bool threadStarted;
    bool destroyRequested;
    bool destroyed;                     // Game thread finished cleaning up
    bool stateSaved;
} AndroidApp;

typedef struct PlatformData {
    AndroidApp *app;
    bool windowReady;                   // Vulkan surface can be created
    bool vulkanReady;                   // rlvkInit() succeeded at least once
    char clipboardBuffer[1024];
    char monitorName[64];
} PlatformData;

static AndroidApp androidApp = { 0 };
static PlatformData platform = { 0 };

// The application entry point, provided by the user (plain C main)
extern int main(int argc, char *argv[]);

//----------------------------------------------------------------------------------
// Module internal functions declaration
//----------------------------------------------------------------------------------
static uint64_t CreateSurfaceAndroid(void *vkInstance, void *userData);

static void WriteCommand(AndroidApp *app, int8_t command);
static int8_t ReadCommand(AndroidApp *app);
static void PreExecuteCommand(AndroidApp *app, int8_t command);
static void PostExecuteCommand(AndroidApp *app, int8_t command);
static void ProcessCommand(AndroidApp *app, int8_t command);
static void ProcessInputQueue(AndroidApp *app);
static int32_t ProcessInputEvent(AInputEvent *event);
static void *AppThreadEntry(void *arg);
static void SetPendingWindow(AndroidApp *app, ANativeWindow *window);
static void SetPendingInputQueue(AndroidApp *app, AInputQueue *queue);
static void SetActivityState(AndroidApp *app, int8_t command);
static void FreeSavedState(AndroidApp *app);

static int TranslateKeyCode(int32_t keyCode);
static int TranslateGamepadButton(int32_t keyCode);
static int DeriveCodepoint(int32_t keyCode, int32_t metaState);
static void UpdateWindowMetrics(void);
static bool WaitForWindow(void);

static JNIEnv *AttachJni(bool *attached);
static void DetachJni(bool attached);

//----------------------------------------------------------------------------------
// Glue: command pipe
//----------------------------------------------------------------------------------
static void WriteCommand(AndroidApp *app, int8_t command)
{
    ssize_t written = 0;
    do { written = write(app->writePipe, &command, sizeof(command)); }
    while ((written < 0) && (errno == EINTR));

    if (written != (ssize_t)sizeof(command))
    {
        __android_log_print(ANDROID_LOG_ERROR, "rayvulkan", "PLATFORM: Failed to write activity command");
    }
}

static int8_t ReadCommand(AndroidApp *app)
{
    int8_t command = 0;
    ssize_t bytes = 0;

    do { bytes = read(app->readPipe, &command, sizeof(command)); }
    while ((bytes < 0) && (errno == EINTR));

    if (bytes != (ssize_t)sizeof(command)) return -1;

    return command;
}

static void FreeSavedState(AndroidApp *app)
{
    pthread_mutex_lock(&app->mutex);
    if (app->savedState != NULL)
    {
        RL_FREE(app->savedState);
        app->savedState = NULL;
        app->savedStateSize = 0;
    }
    pthread_mutex_unlock(&app->mutex);
}

// Game thread: take ownership of whatever the UI thread staged for this command
static void PreExecuteCommand(AndroidApp *app, int8_t command)
{
    switch (command)
    {
        case APP_CMD_INPUT_CHANGED:
        {
            pthread_mutex_lock(&app->mutex);
            if (app->inputQueue != NULL) AInputQueue_detachLooper(app->inputQueue);
            app->inputQueue = app->pendingInputQueue;
            if (app->inputQueue != NULL)
            {
                AInputQueue_attachLooper(app->inputQueue, app->looper, LOOPER_ID_INPUT, NULL, NULL);
            }
            pthread_cond_broadcast(&app->cond);
            pthread_mutex_unlock(&app->mutex);
            break;
        }
        case APP_CMD_INIT_WINDOW:
        {
            pthread_mutex_lock(&app->mutex);
            app->window = app->pendingWindow;
            pthread_cond_broadcast(&app->cond);
            pthread_mutex_unlock(&app->mutex);
            break;
        }
        case APP_CMD_TERM_WINDOW: break;       // Window is released in PostExecuteCommand
        case APP_CMD_CONFIG_CHANGED:
        {
            AConfiguration_fromAssetManager(app->config, app->activity->assetManager);
            break;
        }
        default: break;
    }
}

// Game thread: release resources and unblock the UI thread once the command was handled
static void PostExecuteCommand(AndroidApp *app, int8_t command)
{
    switch (command)
    {
        case APP_CMD_TERM_WINDOW:
        {
            pthread_mutex_lock(&app->mutex);
            app->window = NULL;
            pthread_cond_broadcast(&app->cond);
            pthread_mutex_unlock(&app->mutex);
            break;
        }
        case APP_CMD_SAVE_STATE:
        {
            pthread_mutex_lock(&app->mutex);
            app->stateSaved = true;
            pthread_cond_broadcast(&app->cond);
            pthread_mutex_unlock(&app->mutex);
            break;
        }
        case APP_CMD_RESUME: FreeSavedState(app); break;
        default: break;
    }
}

// Game thread: translate a glue command into CORE state
static void ProcessCommand(AndroidApp *app, int8_t command)
{
    switch (command)
    {
        case APP_CMD_INIT_WINDOW:
        {
            if (app->window == NULL) break;

            platform.windowReady = true;
            UpdateWindowMetrics();

            if (platform.vulkanReady) rlvkResize(CORE.window.render.width, CORE.window.render.height);
            TRACELOG(LOG_INFO, "PLATFORM: Native window created (%i x %i)", CORE.window.render.width, CORE.window.render.height);
            break;
        }
        case APP_CMD_TERM_WINDOW:
        {
            platform.windowReady = false;
            CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
            TRACELOG(LOG_INFO, "PLATFORM: Native window destroyed");
            break;
        }
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        {
            if (!platform.windowReady) break;

            int previousWidth = CORE.window.render.width;
            int previousHeight = CORE.window.render.height;
            UpdateWindowMetrics();

            if ((CORE.window.render.width != previousWidth) || (CORE.window.render.height != previousHeight))
            {
                CORE.window.resizedLastFrame = true;
                if (platform.vulkanReady && rlvkIsReady()) rlvkResize(CORE.window.render.width, CORE.window.render.height);
            }
            break;
        }
        case APP_CMD_GAINED_FOCUS:
        {
            CORE.window.flags &= ~FLAG_WINDOW_UNFOCUSED;
            break;
        }
        case APP_CMD_LOST_FOCUS:
        {
            CORE.window.flags |= FLAG_WINDOW_UNFOCUSED;
            for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) CORE.input.keyboard.currentKeyState[i] = 0;
            for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.input.touch.currentTouchState[i] = 0;
            CORE.input.touch.pointCount = 0;
            break;
        }
        case APP_CMD_RESUME:
        {
            CORE.window.flags &= ~FLAG_WINDOW_MINIMIZED;
            break;
        }
        case APP_CMD_PAUSE:
        case APP_CMD_STOP:
        {
            CORE.window.flags |= FLAG_WINDOW_MINIMIZED;
            break;
        }
        case APP_CMD_DESTROY:
        {
            CORE.window.shouldClose = true;
            break;
        }
        case APP_CMD_LOW_MEMORY:
        {
            TRACELOG(LOG_WARNING, "PLATFORM: System reports low memory");
            break;
        }
        default: break;
    }
}

//----------------------------------------------------------------------------------
// Glue: activity callbacks (UI thread)
//----------------------------------------------------------------------------------
static void SetPendingWindow(AndroidApp *app, ANativeWindow *window)
{
    pthread_mutex_lock(&app->mutex);

    if (app->pendingWindow != NULL) WriteCommand(app, APP_CMD_TERM_WINDOW);
    app->pendingWindow = window;
    if (window != NULL) WriteCommand(app, APP_CMD_INIT_WINDOW);

    while (app->window != app->pendingWindow) pthread_cond_wait(&app->cond, &app->mutex);

    pthread_mutex_unlock(&app->mutex);
}

static void SetPendingInputQueue(AndroidApp *app, AInputQueue *queue)
{
    pthread_mutex_lock(&app->mutex);

    app->pendingInputQueue = queue;
    WriteCommand(app, APP_CMD_INPUT_CHANGED);

    while (app->inputQueue != app->pendingInputQueue) pthread_cond_wait(&app->cond, &app->mutex);

    pthread_mutex_unlock(&app->mutex);
}

static void SetActivityState(AndroidApp *app, int8_t command)
{
    pthread_mutex_lock(&app->mutex);
    WriteCommand(app, command);
    pthread_mutex_unlock(&app->mutex);
}

static void OnStart(ANativeActivity *activity) { SetActivityState((AndroidApp *)activity->instance, APP_CMD_START); }
static void OnResume(ANativeActivity *activity) { SetActivityState((AndroidApp *)activity->instance, APP_CMD_RESUME); }
static void OnPause(ANativeActivity *activity) { SetActivityState((AndroidApp *)activity->instance, APP_CMD_PAUSE); }
static void OnStop(ANativeActivity *activity) { SetActivityState((AndroidApp *)activity->instance, APP_CMD_STOP); }
static void OnLowMemory(ANativeActivity *activity) { SetActivityState((AndroidApp *)activity->instance, APP_CMD_LOW_MEMORY); }

static void OnWindowFocusChanged(ANativeActivity *activity, int focused)
{
    SetActivityState((AndroidApp *)activity->instance, focused? APP_CMD_GAINED_FOCUS : APP_CMD_LOST_FOCUS);
}

static void OnConfigurationChanged(ANativeActivity *activity)
{
    SetActivityState((AndroidApp *)activity->instance, APP_CMD_CONFIG_CHANGED);
}

static void OnNativeWindowCreated(ANativeActivity *activity, ANativeWindow *window)
{
    SetPendingWindow((AndroidApp *)activity->instance, window);
}

static void OnNativeWindowDestroyed(ANativeActivity *activity, ANativeWindow *window)
{
    (void)window;
    SetPendingWindow((AndroidApp *)activity->instance, NULL);
}

static void OnNativeWindowResized(ANativeActivity *activity, ANativeWindow *window)
{
    (void)window;
    SetActivityState((AndroidApp *)activity->instance, APP_CMD_WINDOW_RESIZED);
}

static void OnNativeWindowRedrawNeeded(ANativeActivity *activity, ANativeWindow *window)
{
    (void)window;
    SetActivityState((AndroidApp *)activity->instance, APP_CMD_WINDOW_REDRAW);
}

static void OnInputQueueCreated(ANativeActivity *activity, AInputQueue *queue)
{
    SetPendingInputQueue((AndroidApp *)activity->instance, queue);
}

static void OnInputQueueDestroyed(ANativeActivity *activity, AInputQueue *queue)
{
    (void)queue;
    SetPendingInputQueue((AndroidApp *)activity->instance, NULL);
}

static void *OnSaveInstanceState(ANativeActivity *activity, size_t *outSize)
{
    AndroidApp *app = (AndroidApp *)activity->instance;
    void *state = NULL;

    pthread_mutex_lock(&app->mutex);
    app->stateSaved = false;
    WriteCommand(app, APP_CMD_SAVE_STATE);
    while (!app->stateSaved) pthread_cond_wait(&app->cond, &app->mutex);

    if (app->savedState != NULL)
    {
        state = app->savedState;
        *outSize = app->savedStateSize;
        app->savedState = NULL;
        app->savedStateSize = 0;
    }
    else *outSize = 0;

    pthread_mutex_unlock(&app->mutex);

    return state;
}

static void OnDestroy(ANativeActivity *activity)
{
    AndroidApp *app = (AndroidApp *)activity->instance;

    pthread_mutex_lock(&app->mutex);
    app->destroyRequested = true;
    WriteCommand(app, APP_CMD_DESTROY);
    while (!app->destroyed) pthread_cond_wait(&app->cond, &app->mutex);
    pthread_mutex_unlock(&app->mutex);

    close(app->readPipe);
    close(app->writePipe);
    if (app->config != NULL) AConfiguration_delete(app->config);
    pthread_cond_destroy(&app->cond);
    pthread_mutex_destroy(&app->mutex);
}

//----------------------------------------------------------------------------------
// Glue: game thread
//----------------------------------------------------------------------------------
static void *AppThreadEntry(void *arg)
{
    AndroidApp *app = (AndroidApp *)arg;

    app->config = AConfiguration_new();
    AConfiguration_fromAssetManager(app->config, app->activity->assetManager);

    app->looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ALooper_addFd(app->looper, app->readPipe, LOOPER_ID_MAIN, ALOOPER_EVENT_INPUT, NULL, NULL);

    pthread_mutex_lock(&app->mutex);
    app->threadStarted = true;
    pthread_cond_broadcast(&app->cond);
    pthread_mutex_unlock(&app->mutex);

    char programName[] = "rayvulkan";
    char *argv[] = { programName, NULL };
    main(1, argv);

    // The game returned from main(): tell the framework to finish the activity
    pthread_mutex_lock(&app->mutex);
    app->destroyed = true;
    pthread_cond_broadcast(&app->cond);
    pthread_mutex_unlock(&app->mutex);

    if (!app->destroyRequested) ANativeActivity_finish(app->activity);

    return NULL;
}

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *activity, void *savedState, size_t savedStateSize)
{
    memset(&androidApp, 0, sizeof(androidApp));
    androidApp.activity = activity;

    pthread_mutex_init(&androidApp.mutex, NULL);
    pthread_cond_init(&androidApp.cond, NULL);

    if ((savedState != NULL) && (savedStateSize > 0))
    {
        androidApp.savedState = RL_MALLOC(savedStateSize);
        if (androidApp.savedState != NULL)
        {
            memcpy(androidApp.savedState, savedState, savedStateSize);
            androidApp.savedStateSize = savedStateSize;
        }
    }

    int pipeFds[2] = { -1, -1 };
    if (pipe(pipeFds) != 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, "rayvulkan", "PLATFORM: Failed to create activity command pipe");
        return;
    }
    androidApp.readPipe = pipeFds[0];
    androidApp.writePipe = pipeFds[1];

    activity->instance = &androidApp;
    activity->callbacks->onStart = OnStart;
    activity->callbacks->onResume = OnResume;
    activity->callbacks->onPause = OnPause;
    activity->callbacks->onStop = OnStop;
    activity->callbacks->onDestroy = OnDestroy;
    activity->callbacks->onSaveInstanceState = OnSaveInstanceState;
    activity->callbacks->onWindowFocusChanged = OnWindowFocusChanged;
    activity->callbacks->onNativeWindowCreated = OnNativeWindowCreated;
    activity->callbacks->onNativeWindowDestroyed = OnNativeWindowDestroyed;
    activity->callbacks->onNativeWindowResized = OnNativeWindowResized;
    activity->callbacks->onNativeWindowRedrawNeeded = OnNativeWindowRedrawNeeded;
    activity->callbacks->onInputQueueCreated = OnInputQueueCreated;
    activity->callbacks->onInputQueueDestroyed = OnInputQueueDestroyed;
    activity->callbacks->onConfigurationChanged = OnConfigurationChanged;
    activity->callbacks->onLowMemory = OnLowMemory;

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&androidApp.thread, &attributes, AppThreadEntry, &androidApp) != 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, "rayvulkan", "PLATFORM: Failed to start game thread");
        pthread_attr_destroy(&attributes);
        return;
    }
    pthread_attr_destroy(&attributes);

    pthread_mutex_lock(&androidApp.mutex);
    while (!androidApp.threadStarted) pthread_cond_wait(&androidApp.cond, &androidApp.mutex);
    pthread_mutex_unlock(&androidApp.mutex);
}

// Compatibility entry point for NDK build scripts expecting the glue signature
void android_main(void *app)
{
    (void)app;

    char programName[] = "rayvulkan";
    char *argv[] = { programName, NULL };
    main(1, argv);
}

//----------------------------------------------------------------------------------
// Input translation
//----------------------------------------------------------------------------------
static int TranslateKeyCode(int32_t keyCode)
{
    switch (keyCode)
    {
        case AKEYCODE_BACK: return KEY_BACK;
        case AKEYCODE_MENU: return KEY_MENU;
        case AKEYCODE_VOLUME_UP: return KEY_VOLUME_UP;
        case AKEYCODE_VOLUME_DOWN: return KEY_VOLUME_DOWN;

        case AKEYCODE_SPACE: return KEY_SPACE;
        case AKEYCODE_ESCAPE: return KEY_ESCAPE;
        case AKEYCODE_ENTER: return KEY_ENTER;
        case AKEYCODE_TAB: return KEY_TAB;
        case AKEYCODE_DEL: return KEY_BACKSPACE;
        case AKEYCODE_INSERT: return KEY_INSERT;
        case AKEYCODE_FORWARD_DEL: return KEY_DELETE;
        case AKEYCODE_DPAD_RIGHT: return KEY_RIGHT;
        case AKEYCODE_DPAD_LEFT: return KEY_LEFT;
        case AKEYCODE_DPAD_DOWN: return KEY_DOWN;
        case AKEYCODE_DPAD_UP: return KEY_UP;
        case AKEYCODE_PAGE_UP: return KEY_PAGE_UP;
        case AKEYCODE_PAGE_DOWN: return KEY_PAGE_DOWN;
        case AKEYCODE_MOVE_HOME: return KEY_HOME;
        case AKEYCODE_MOVE_END: return KEY_END;
        case AKEYCODE_CAPS_LOCK: return KEY_CAPS_LOCK;
        case AKEYCODE_SCROLL_LOCK: return KEY_SCROLL_LOCK;
        case AKEYCODE_NUM_LOCK: return KEY_NUM_LOCK;
        case AKEYCODE_SYSRQ: return KEY_PRINT_SCREEN;
        case AKEYCODE_BREAK: return KEY_PAUSE;

        case AKEYCODE_F1: return KEY_F1;
        case AKEYCODE_F2: return KEY_F2;
        case AKEYCODE_F3: return KEY_F3;
        case AKEYCODE_F4: return KEY_F4;
        case AKEYCODE_F5: return KEY_F5;
        case AKEYCODE_F6: return KEY_F6;
        case AKEYCODE_F7: return KEY_F7;
        case AKEYCODE_F8: return KEY_F8;
        case AKEYCODE_F9: return KEY_F9;
        case AKEYCODE_F10: return KEY_F10;
        case AKEYCODE_F11: return KEY_F11;
        case AKEYCODE_F12: return KEY_F12;

        case AKEYCODE_SHIFT_LEFT: return KEY_LEFT_SHIFT;
        case AKEYCODE_SHIFT_RIGHT: return KEY_RIGHT_SHIFT;
        case AKEYCODE_CTRL_LEFT: return KEY_LEFT_CONTROL;
        case AKEYCODE_CTRL_RIGHT: return KEY_RIGHT_CONTROL;
        case AKEYCODE_ALT_LEFT: return KEY_LEFT_ALT;
        case AKEYCODE_ALT_RIGHT: return KEY_RIGHT_ALT;
        case AKEYCODE_META_LEFT: return KEY_LEFT_SUPER;
        case AKEYCODE_META_RIGHT: return KEY_RIGHT_SUPER;

        case AKEYCODE_GRAVE: return KEY_GRAVE;
        case AKEYCODE_MINUS: return KEY_MINUS;
        case AKEYCODE_EQUALS: return KEY_EQUAL;
        case AKEYCODE_LEFT_BRACKET: return KEY_LEFT_BRACKET;
        case AKEYCODE_RIGHT_BRACKET: return KEY_RIGHT_BRACKET;
        case AKEYCODE_BACKSLASH: return KEY_BACKSLASH;
        case AKEYCODE_SEMICOLON: return KEY_SEMICOLON;
        case AKEYCODE_APOSTROPHE: return KEY_APOSTROPHE;
        case AKEYCODE_COMMA: return KEY_COMMA;
        case AKEYCODE_PERIOD: return KEY_PERIOD;
        case AKEYCODE_SLASH: return KEY_SLASH;

        case AKEYCODE_NUMPAD_0: return KEY_KP_0;
        case AKEYCODE_NUMPAD_1: return KEY_KP_1;
        case AKEYCODE_NUMPAD_2: return KEY_KP_2;
        case AKEYCODE_NUMPAD_3: return KEY_KP_3;
        case AKEYCODE_NUMPAD_4: return KEY_KP_4;
        case AKEYCODE_NUMPAD_5: return KEY_KP_5;
        case AKEYCODE_NUMPAD_6: return KEY_KP_6;
        case AKEYCODE_NUMPAD_7: return KEY_KP_7;
        case AKEYCODE_NUMPAD_8: return KEY_KP_8;
        case AKEYCODE_NUMPAD_9: return KEY_KP_9;
        case AKEYCODE_NUMPAD_DOT: return KEY_KP_DECIMAL;
        case AKEYCODE_NUMPAD_DIVIDE: return KEY_KP_DIVIDE;
        case AKEYCODE_NUMPAD_MULTIPLY: return KEY_KP_MULTIPLY;
        case AKEYCODE_NUMPAD_SUBTRACT: return KEY_KP_SUBTRACT;
        case AKEYCODE_NUMPAD_ADD: return KEY_KP_ADD;
        case AKEYCODE_NUMPAD_ENTER: return KEY_KP_ENTER;
        case AKEYCODE_NUMPAD_EQUALS: return KEY_KP_EQUAL;
        default: break;
    }

    if ((keyCode >= AKEYCODE_0) && (keyCode <= AKEYCODE_9)) return KEY_ZERO + (int)(keyCode - AKEYCODE_0);
    if ((keyCode >= AKEYCODE_A) && (keyCode <= AKEYCODE_Z)) return KEY_A + (int)(keyCode - AKEYCODE_A);

    return KEY_NULL;
}

static int TranslateGamepadButton(int32_t keyCode)
{
    switch (keyCode)
    {
        case AKEYCODE_BUTTON_A: return GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
        case AKEYCODE_BUTTON_B: return GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
        case AKEYCODE_BUTTON_X: return GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
        case AKEYCODE_BUTTON_Y: return GAMEPAD_BUTTON_RIGHT_FACE_UP;
        case AKEYCODE_BUTTON_L1: return GAMEPAD_BUTTON_LEFT_TRIGGER_1;
        case AKEYCODE_BUTTON_R1: return GAMEPAD_BUTTON_RIGHT_TRIGGER_1;
        case AKEYCODE_BUTTON_L2: return GAMEPAD_BUTTON_LEFT_TRIGGER_2;
        case AKEYCODE_BUTTON_R2: return GAMEPAD_BUTTON_RIGHT_TRIGGER_2;
        case AKEYCODE_BUTTON_THUMBL: return GAMEPAD_BUTTON_LEFT_THUMB;
        case AKEYCODE_BUTTON_THUMBR: return GAMEPAD_BUTTON_RIGHT_THUMB;
        case AKEYCODE_BUTTON_SELECT: return GAMEPAD_BUTTON_MIDDLE_LEFT;
        case AKEYCODE_BUTTON_MODE: return GAMEPAD_BUTTON_MIDDLE;
        case AKEYCODE_BUTTON_START: return GAMEPAD_BUTTON_MIDDLE_RIGHT;
        case AKEYCODE_DPAD_UP: return GAMEPAD_BUTTON_LEFT_FACE_UP;
        case AKEYCODE_DPAD_RIGHT: return GAMEPAD_BUTTON_LEFT_FACE_RIGHT;
        case AKEYCODE_DPAD_DOWN: return GAMEPAD_BUTTON_LEFT_FACE_DOWN;
        case AKEYCODE_DPAD_LEFT: return GAMEPAD_BUTTON_LEFT_FACE_LEFT;
        default: break;
    }

    return GAMEPAD_BUTTON_UNKNOWN;
}

// Best effort ASCII for the char queue: Android reports key codes, the unicode value
// would require a JNI call into KeyEvent.getUnicodeChar() for every keystroke
static int DeriveCodepoint(int32_t keyCode, int32_t metaState)
{
    bool shift = ((metaState & AMETA_SHIFT_ON) != 0);

    if ((keyCode >= AKEYCODE_A) && (keyCode <= AKEYCODE_Z))
    {
        int letter = (int)(keyCode - AKEYCODE_A);
        return shift? ('A' + letter) : ('a' + letter);
    }

    if ((keyCode >= AKEYCODE_0) && (keyCode <= AKEYCODE_9) && !shift) return '0' + (int)(keyCode - AKEYCODE_0);

    switch (keyCode)
    {
        case AKEYCODE_SPACE: return ' ';
        case AKEYCODE_COMMA: return shift? '<' : ',';
        case AKEYCODE_PERIOD: return shift? '>' : '.';
        case AKEYCODE_MINUS: return shift? '_' : '-';
        case AKEYCODE_EQUALS: return shift? '+' : '=';
        case AKEYCODE_SLASH: return shift? '?' : '/';
        case AKEYCODE_BACKSLASH: return shift? '|' : '\\';
        case AKEYCODE_SEMICOLON: return shift? ':' : ';';
        case AKEYCODE_APOSTROPHE: return shift? '"' : '\'';
        case AKEYCODE_LEFT_BRACKET: return shift? '{' : '[';
        case AKEYCODE_RIGHT_BRACKET: return shift? '}' : ']';
        case AKEYCODE_GRAVE: return shift? '~' : '`';
        default: break;
    }

    return 0;
}

// Feed a touch event into CORE.input.touch (multitouch, pointer id preserving)
static void ProcessTouchEvent(AInputEvent *event)
{
    int32_t action = AMotionEvent_getAction(event);
    int32_t flags = action & AMOTION_EVENT_ACTION_MASK;
    int32_t pointerIndex = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                           AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    size_t pointerCount = AMotionEvent_getPointerCount(event);
    if (pointerCount > MAX_TOUCH_POINTS) pointerCount = MAX_TOUCH_POINTS;

    float scaleX = (CORE.window.render.width > 0)?
        ((float)CORE.window.screen.width/(float)CORE.window.render.width) : 1.0f;
    float scaleY = (CORE.window.render.height > 0)?
        ((float)CORE.window.screen.height/(float)CORE.window.render.height) : 1.0f;

    for (size_t i = 0; i < pointerCount; i++)
    {
        CORE.input.touch.pointId[i] = AMotionEvent_getPointerId(event, i);
        CORE.input.touch.position[i].x = AMotionEvent_getX(event, i)*scaleX;
        CORE.input.touch.position[i].y = AMotionEvent_getY(event, i)*scaleY;
        CORE.input.touch.currentTouchState[i] = 1;
    }

    for (size_t i = pointerCount; i < MAX_TOUCH_POINTS; i++) CORE.input.touch.currentTouchState[i] = 0;

    if ((flags == AMOTION_EVENT_ACTION_POINTER_UP) || (flags == AMOTION_EVENT_ACTION_UP))
    {
        if ((pointerIndex >= 0) && (pointerIndex < (int32_t)pointerCount))
        {
            CORE.input.touch.currentTouchState[pointerIndex] = 0;
        }
    }

    if ((flags == AMOTION_EVENT_ACTION_UP) || (flags == AMOTION_EVENT_ACTION_CANCEL))
    {
        for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.input.touch.currentTouchState[i] = 0;
    }

    int count = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) if (CORE.input.touch.currentTouchState[i] == 1) count++;
    CORE.input.touch.pointCount = count;

    // Mirror the first touch point onto the mouse so mouse based code keeps working
    CORE.input.mouse.currentPosition = CORE.input.touch.position[0];
    CORE.input.mouse.currentButtonState[MOUSE_BUTTON_LEFT] = (count > 0)? 1 : 0;
    CORE.input.mouse.cursorOnScreen = (count > 0);
}

// Feed a gamepad stick/trigger event into CORE.input.gamepad axes
static void ProcessGamepadMotion(AInputEvent *event)
{
    const int gamepad = 0;      // NativeActivity does not expose a stable device index mapping

    CORE.input.gamepad.ready[gamepad] = true;
    CORE.input.gamepad.axisCount[gamepad] = 6;
    if (CORE.input.gamepad.name[gamepad][0] == '\0') TextCopy(CORE.input.gamepad.name[gamepad], "Android Gamepad");

    CORE.input.gamepad.axisState[gamepad][GAMEPAD_AXIS_LEFT_X] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
    CORE.input.gamepad.axisState[gamepad][GAMEPAD_AXIS_LEFT_Y] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
    CORE.input.gamepad.axisState[gamepad][GAMEPAD_AXIS_RIGHT_X] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
    CORE.input.gamepad.axisState[gamepad][GAMEPAD_AXIS_RIGHT_Y] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);

    // Triggers are reported in [0..1] and raylib expects [-1..1]
    float leftTrigger = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0);
    float rightTrigger = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);
    CORE.input.gamepad.axisState[gamepad][GAMEPAD_AXIS_LEFT_TRIGGER] = leftTrigger*2.0f - 1.0f;
    CORE.input.gamepad.axisState[gamepad][GAMEPAD_AXIS_RIGHT_TRIGGER] = rightTrigger*2.0f - 1.0f;

    // Hat switch reported as a motion axis on many pads
    float hatX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
    float hatY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
    CORE.input.gamepad.currentButtonState[gamepad][GAMEPAD_BUTTON_LEFT_FACE_LEFT] = (hatX < -0.5f)? 1 : 0;
    CORE.input.gamepad.currentButtonState[gamepad][GAMEPAD_BUTTON_LEFT_FACE_RIGHT] = (hatX > 0.5f)? 1 : 0;
    CORE.input.gamepad.currentButtonState[gamepad][GAMEPAD_BUTTON_LEFT_FACE_UP] = (hatY < -0.5f)? 1 : 0;
    CORE.input.gamepad.currentButtonState[gamepad][GAMEPAD_BUTTON_LEFT_FACE_DOWN] = (hatY > 0.5f)? 1 : 0;
}

// Returns 1 when the event was consumed (never let the back key close the app silently)
static int32_t ProcessInputEvent(AInputEvent *event)
{
    const int32_t type = AInputEvent_getType(event);
    const int32_t source = AInputEvent_getSource(event);

    if (type == AINPUT_EVENT_TYPE_MOTION)
    {
        if (((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) ||
            ((source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD))
        {
            ProcessGamepadMotion(event);
            return 1;
        }

        ProcessTouchEvent(event);
        return 1;
    }

    if (type != AINPUT_EVENT_TYPE_KEY) return 0;

    const int32_t keyCode = AKeyEvent_getKeyCode(event);
    const int32_t action = AKeyEvent_getAction(event);
    const bool down = (action == AKEY_EVENT_ACTION_DOWN);

    if (((source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD) ||
        ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK))
    {
        int button = TranslateGamepadButton(keyCode);
        if (button != GAMEPAD_BUTTON_UNKNOWN)
        {
            CORE.input.gamepad.ready[0] = true;
            if (CORE.input.gamepad.axisCount[0] == 0) CORE.input.gamepad.axisCount[0] = 6;
            if (CORE.input.gamepad.name[0][0] == '\0') TextCopy(CORE.input.gamepad.name[0], "Android Gamepad");

            CORE.input.gamepad.currentButtonState[0][button] = down? 1 : 0;
            if (down) CORE.input.gamepad.lastButtonPressed = button;
            return 1;
        }
    }

    int key = TranslateKeyCode(keyCode);
    if ((key <= 0) || (key >= MAX_KEYBOARD_KEYS)) return 0;

    if (down)
    {
        if (AKeyEvent_getRepeatCount(event) > 0) CORE.input.keyboard.keyRepeatInFrame[key] = 1;

        CORE.input.keyboard.currentKeyState[key] = 1;

        if (CORE.input.keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
        {
            CORE.input.keyboard.keyPressedQueue[CORE.input.keyboard.keyPressedQueueCount] = key;
            CORE.input.keyboard.keyPressedQueueCount++;
        }

        int codepoint = DeriveCodepoint(keyCode, AKeyEvent_getMetaState(event));
        if ((codepoint > 0) && (CORE.input.keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE))
        {
            CORE.input.keyboard.charPressedQueue[CORE.input.keyboard.charPressedQueueCount] = codepoint;
            CORE.input.keyboard.charPressedQueueCount++;
        }
    }
    else CORE.input.keyboard.currentKeyState[key] = 0;

    // The back key is delivered to the app (raylib exposes it as KEY_BACK) instead of
    // being handled by the system, unless it is also the configured exit key
    if ((keyCode == AKEYCODE_BACK) && (CORE.input.keyboard.exitKey == KEY_BACK) && !down)
    {
        CORE.window.shouldClose = true;
    }

    return 1;
}

static void ProcessInputQueue(AndroidApp *app)
{
    if (app->inputQueue == NULL) return;

    AInputEvent *event = NULL;
    while (AInputQueue_getEvent(app->inputQueue, &event) >= 0)
    {
        if (AInputQueue_preDispatchEvent(app->inputQueue, event) != 0) continue;   // Handled by the IME

        int32_t handled = ProcessInputEvent(event);
        AInputQueue_finishEvent(app->inputQueue, event, handled);
    }
}

//----------------------------------------------------------------------------------
// Window metrics
//----------------------------------------------------------------------------------
static void UpdateWindowMetrics(void)
{
    AndroidApp *app = platform.app;
    if ((app == NULL) || (app->window == NULL)) return;

    int width = ANativeWindow_getWidth(app->window);
    int height = ANativeWindow_getHeight(app->window);
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    CORE.window.render.width = width;
    CORE.window.render.height = height;
    CORE.window.display.width = width;
    CORE.window.display.height = height;

    // Screen size stays what the user requested through InitWindow(); rendering is scaled
    // by rcore through the mouse offset/scale. When nothing was requested, match the display
    if ((CORE.window.screen.width <= 0) || (CORE.window.screen.height <= 0) ||
        ((CORE.window.flags & FLAG_WINDOW_HIGHDPI) > 0))
    {
        CORE.window.screen.width = width;
        CORE.window.screen.height = height;
    }

    int density = (app->config != NULL)? (int)AConfiguration_getDensity(app->config) : 160;
    if (density <= 0) density = 160;

    CORE.window.scale.x = (float)density/160.0f;
    CORE.window.scale.y = CORE.window.scale.x;

    CORE.window.position.x = 0;
    CORE.window.position.y = 0;
}

// Pump commands until the framework handed us a usable native window
static bool WaitForWindow(void)
{
    AndroidApp *app = platform.app;

    while (!platform.windowReady && !CORE.window.shouldClose)
    {
        int events = 0;
        int identifier = ALooper_pollOnce(-1, NULL, &events, NULL);

        if (identifier == LOOPER_ID_MAIN)
        {
            int8_t command = ReadCommand(app);
            if (command < 0) return false;

            PreExecuteCommand(app, command);
            ProcessCommand(app, command);
            PostExecuteCommand(app, command);
        }
        else if (identifier == LOOPER_ID_INPUT) ProcessInputQueue(app);
        else if (identifier == ALOOPER_POLL_ERROR) return false;
    }

    return platform.windowReady;
}

//----------------------------------------------------------------------------------
// Vulkan surface glue
//----------------------------------------------------------------------------------
static uint64_t CreateSurfaceAndroid(void *vkInstance, void *userData)
{
    (void)userData;

    VkInstance instance = (VkInstance)vkInstance;
    AndroidApp *app = platform.app;
    if ((instance == VK_NULL_HANDLE) || (app == NULL) || (app->window == NULL)) return 0;

    PFN_vkCreateAndroidSurfaceKHR createSurface =
        (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR");
    if (createSurface == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: vkCreateAndroidSurfaceKHR not available");
        return 0;
    }

    VkAndroidSurfaceCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .pNext = NULL,
        .flags = 0,
        .window = app->window
    };

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (createSurface(instance, &createInfo, NULL, &surface) != VK_SUCCESS)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to create Vulkan Android surface");
        return 0;
    }

#if defined(VK_USE_64_BIT_PTR_DEFINES) && (VK_USE_64_BIT_PTR_DEFINES == 1)
    return (uint64_t)(uintptr_t)surface;
#else
    return (uint64_t)surface;
#endif
}

//----------------------------------------------------------------------------------
// JNI helpers (all failures are warnings, never fatal)
//----------------------------------------------------------------------------------
static JNIEnv *AttachJni(bool *attached)
{
    *attached = false;

    AndroidApp *app = platform.app;
    if ((app == NULL) || (app->activity == NULL) || (app->activity->vm == NULL)) return NULL;

    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;

    jint result = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (result == JNI_OK) return env;

    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to attach thread to the Java VM");
        return NULL;
    }

    *attached = true;

    return env;
}

static void DetachJni(bool attached)
{
    if (!attached) return;

    AndroidApp *app = platform.app;
    if ((app == NULL) || (app->activity == NULL) || (app->activity->vm == NULL)) return;

    (*app->activity->vm)->DetachCurrentThread(app->activity->vm);
}

// Get the android.content.ClipboardManager instance, NULL on any failure
static jobject GetClipboardManager(JNIEnv *env)
{
    AndroidApp *app = platform.app;

    jclass activityClass = (*env)->GetObjectClass(env, app->activity->clazz);
    if (activityClass == NULL) return NULL;

    jmethodID getSystemService = (*env)->GetMethodID(env, activityClass, "getSystemService",
                                                     "(Ljava/lang/String;)Ljava/lang/Object;");
    if (getSystemService == NULL)
    {
        (*env)->ExceptionClear(env);
        return NULL;
    }

    jstring serviceName = (*env)->NewStringUTF(env, "clipboard");
    if (serviceName == NULL) return NULL;

    jobject manager = (*env)->CallObjectMethod(env, app->activity->clazz, getSystemService, serviceName);
    (*env)->DeleteLocalRef(env, serviceName);

    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionClear(env);
        return NULL;
    }

    return manager;
}

//----------------------------------------------------------------------------------
// Platform contract: initialization
//----------------------------------------------------------------------------------
int InitPlatform(void)
{
    memset(&platform, 0, sizeof(platform));
    platform.app = &androidApp;

    if (androidApp.activity == NULL)
    {
        TRACELOG(LOG_ERROR, "PLATFORM: NativeActivity not initialized (missing ANativeActivity_onCreate)");
        return -1;
    }

    // Fullscreen (and keep the screen on) is the sane default for a game
    unsigned int addFlags = AWINDOW_FLAG_KEEP_SCREEN_ON;
    if ((CORE.window.flags & (FLAG_FULLSCREEN_MODE|FLAG_BORDERLESS_WINDOWED_MODE)) > 0)
    {
        addFlags |= AWINDOW_FLAG_FULLSCREEN;
        CORE.window.fullscreen = true;
    }
    ANativeActivity_setWindowFlags(androidApp.activity, addFlags, 0);

    if (!WaitForWindow())
    {
        TRACELOG(LOG_ERROR, "PLATFORM: No native window available");
        return -1;
    }

    UpdateWindowMetrics();

    static const char *instanceExtensions[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
    rlvkPlatformGlue glue = {
        .requiredExtensions = instanceExtensions,
        .requiredExtensionCount = 2,
        .createSurface = CreateSurfaceAndroid,
        .userData = NULL,
        .portability = false
    };

    if (!rlvkInit(&glue, CORE.window.render.width, CORE.window.render.height,
                  ((CORE.window.flags & FLAG_VSYNC_HINT) > 0)))
    {
        TRACELOG(LOG_ERROR, "PLATFORM: Failed to initialize Vulkan (rlvk)");
        return -1;
    }
    platform.vulkanReady = true;

    CORE.input.mouse.scale.x = 1.0f;
    CORE.input.mouse.scale.y = 1.0f;
    CORE.input.mouse.cursorHidden = true;       // There is no system cursor on Android

    TRACELOG(LOG_INFO, "PLATFORM: Initialized successfully (Android NativeActivity)");
    TRACELOG(LOG_INFO, "    > Window size:   %i x %i", CORE.window.screen.width, CORE.window.screen.height);
    TRACELOG(LOG_INFO, "    > Render size:   %i x %i", CORE.window.render.width, CORE.window.render.height);
    TRACELOG(LOG_INFO, "    > Display DPI:   %.0f", (double)(CORE.window.scale.x*160.0f));

    return 0;
}

void ClosePlatform(void)
{
    platform.windowReady = false;
    platform.vulkanReady = false;

    AndroidApp *app = platform.app;
    if (app == NULL) return;

    if (app->inputQueue != NULL)
    {
        AInputQueue_detachLooper(app->inputQueue);
        app->inputQueue = NULL;
    }

    TRACELOG(LOG_INFO, "PLATFORM: Closed (Android)");
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

    AndroidApp *app = platform.app;
    if (app == NULL) return;

    // Block while the app is in the background (or when the user asked for event waiting)
    bool paused = ((CORE.window.flags & FLAG_WINDOW_MINIMIZED) > 0) || !platform.windowReady;
    bool block = (CORE.window.eventWaiting || paused) &&
                 ((CORE.window.flags & FLAG_WINDOW_ALWAYS_RUN) == 0) && !CORE.window.shouldClose;

    for (;;)
    {
        int events = 0;
        int identifier = ALooper_pollOnce(block? -1 : 0, NULL, &events, NULL);

        if (identifier == LOOPER_ID_MAIN)
        {
            int8_t command = ReadCommand(app);
            if (command >= 0)
            {
                PreExecuteCommand(app, command);
                ProcessCommand(app, command);
                PostExecuteCommand(app, command);
            }
        }
        else if (identifier == LOOPER_ID_INPUT) ProcessInputQueue(app);
        else break;                             // ALOOPER_POLL_TIMEOUT / WAKE / ERROR

        block = false;                          // Only the first poll may block
        if (CORE.window.shouldClose) break;
    }
}

double GetTimePlatform(void)
{
    struct timespec now = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (double)now.tv_sec + (double)now.tv_nsec*1.0e-9;
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
// Platform contract: window control (mostly not applicable to a single fullscreen surface)
//----------------------------------------------------------------------------------
void SetWindowTitlePlatform(const char *title)
{
    CORE.window.title = title;
    // An Android activity has no title bar under the game surface
}

void SetWindowPositionPlatform(int x, int y)
{
    (void)x;
    (void)y;
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowPosition() not available on Android");
}

void SetWindowSizePlatform(int width, int height)
{
    (void)width;
    (void)height;
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowSize() not available on Android (surface is display sized)");
}

void SetWindowMinSizePlatform(int width, int height)
{
    CORE.window.screenMin.width = width;
    CORE.window.screenMin.height = height;
}

void SetWindowMaxSizePlatform(int width, int height)
{
    CORE.window.screenMax.width = width;
    CORE.window.screenMax.height = height;
}

void SetWindowOpacityPlatform(float opacity)
{
    (void)opacity;
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowOpacity() not available on Android");
}

void SetWindowIconPlatform(Image *images, int count)
{
    (void)images;
    (void)count;
    TRACELOG(LOG_WARNING, "PLATFORM: SetWindowIcon() not available on Android (icon comes from the manifest)");
}

void SetWindowFocusedPlatform(void)
{
    // The activity is focused by the system, nothing to request
}

void SetWindowMonitorPlatform(int monitor)
{
    if (monitor != 0) TRACELOG(LOG_WARNING, "PLATFORM: Android exposes a single display");
}

void SetWindowStatePlatform(unsigned int flags)
{
    CORE.window.flags |= flags;

    if ((platform.app == NULL) || (platform.app->activity == NULL)) return;

    if ((flags & FLAG_VSYNC_HINT) > 0) rlvkSetVSync(true);

    if ((flags & (FLAG_FULLSCREEN_MODE|FLAG_BORDERLESS_WINDOWED_MODE)) > 0)
    {
        ANativeActivity_setWindowFlags(platform.app->activity, AWINDOW_FLAG_FULLSCREEN, 0);
        CORE.window.fullscreen = true;
    }

    if ((flags & FLAG_WINDOW_ALWAYS_RUN) > 0)
    {
        ANativeActivity_setWindowFlags(platform.app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    }
}

void ClearWindowStatePlatform(unsigned int flags)
{
    CORE.window.flags &= ~flags;

    if ((platform.app == NULL) || (platform.app->activity == NULL)) return;

    if ((flags & FLAG_VSYNC_HINT) > 0) rlvkSetVSync(false);

    if ((flags & (FLAG_FULLSCREEN_MODE|FLAG_BORDERLESS_WINDOWED_MODE)) > 0)
    {
        ANativeActivity_setWindowFlags(platform.app->activity, 0, AWINDOW_FLAG_FULLSCREEN);
        CORE.window.fullscreen = false;
    }

    if ((flags & FLAG_WINDOW_ALWAYS_RUN) > 0)
    {
        ANativeActivity_setWindowFlags(platform.app->activity, 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
    }
}

void ToggleFullscreenPlatform(void)
{
    if ((platform.app == NULL) || (platform.app->activity == NULL)) return;

    if (CORE.window.fullscreen)
    {
        ANativeActivity_setWindowFlags(platform.app->activity, 0, AWINDOW_FLAG_FULLSCREEN);
        CORE.window.fullscreen = false;
        CORE.window.flags &= ~FLAG_FULLSCREEN_MODE;
    }
    else
    {
        ANativeActivity_setWindowFlags(platform.app->activity, AWINDOW_FLAG_FULLSCREEN, 0);
        CORE.window.fullscreen = true;
        CORE.window.flags |= FLAG_FULLSCREEN_MODE;
    }

    TRACELOG(LOG_INFO, "PLATFORM: %s fullscreen mode", CORE.window.fullscreen? "Entered" : "Left");
}

void ToggleBorderlessWindowedPlatform(void)
{
    // Identical to fullscreen on Android: the activity always owns the whole display
    ToggleFullscreenPlatform();

    if (CORE.window.fullscreen) CORE.window.flags |= FLAG_BORDERLESS_WINDOWED_MODE;
    else CORE.window.flags &= ~FLAG_BORDERLESS_WINDOWED_MODE;
}

void MaximizeWindowPlatform(void)
{
    // The activity surface is always display sized
}

void MinimizeWindowPlatform(void)
{
    // Sending the activity to the background is the user's decision, not the app's
    TRACELOG(LOG_WARNING, "PLATFORM: MinimizeWindow() not available on Android");
}

void RestoreWindowPlatform(void)
{
    // The activity surface is always display sized
}

void *GetWindowHandlePlatform(void)
{
    return (void *)((platform.app != NULL)? platform.app->window : NULL);
}

//----------------------------------------------------------------------------------
// Platform contract: monitors (Android exposes exactly one display here)
//----------------------------------------------------------------------------------
int GetMonitorCountPlatform(void)
{
    return 1;
}

int GetCurrentMonitorPlatform(void)
{
    return 0;
}

Vector2 GetMonitorPositionPlatform(int monitor)
{
    Vector2 position = { 0.0f, 0.0f };
    if (monitor != 0) TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);

    return position;
}

int GetMonitorWidthPlatform(int monitor)
{
    if (monitor != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return CORE.window.display.width;
}

int GetMonitorHeightPlatform(int monitor)
{
    if (monitor != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return CORE.window.display.height;
}

// Physical size derived from pixels and density (Android has no EDID access)
static int PhysicalSizeMillimetres(int pixels)
{
    float scale = (CORE.window.scale.x > 0.0f)? CORE.window.scale.x : 1.0f;
    float dpi = scale*160.0f;
    if (dpi <= 0.0f) return 0;

    return (int)(((float)pixels/dpi)*25.4f);
}

int GetMonitorPhysicalWidthPlatform(int monitor)
{
    if (monitor != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return PhysicalSizeMillimetres(CORE.window.display.width);
}

int GetMonitorPhysicalHeightPlatform(int monitor)
{
    if (monitor != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    return PhysicalSizeMillimetres(CORE.window.display.height);
}

int GetMonitorRefreshRatePlatform(int monitor)
{
    if (monitor != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return 0;
    }

    // The exact rate needs Display.getRefreshRate() through JNI; 60 Hz is the safe default
    return 60;
}

const char *GetMonitorNamePlatform(int monitor)
{
    platform.monitorName[0] = '\0';

    if (monitor != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Selected monitor not found (%i)", monitor);
        return platform.monitorName;
    }

    TextCopy(platform.monitorName, "Android Display");

    return platform.monitorName;
}

Vector2 GetWindowScaleDPIPlatform(void)
{
    Vector2 scale = { CORE.window.scale.x, CORE.window.scale.y };
    if (scale.x <= 0.0f) scale.x = 1.0f;
    if (scale.y <= 0.0f) scale.y = 1.0f;

    return scale;
}

//----------------------------------------------------------------------------------
// Platform contract: cursor (touch devices have no pointer)
//----------------------------------------------------------------------------------
void ShowCursorPlatform(void)
{
    CORE.input.mouse.cursorHidden = false;
}

void HideCursorPlatform(void)
{
    CORE.input.mouse.cursorHidden = true;
}

void EnableCursorPlatform(void)
{
    CORE.input.mouse.cursorLocked = false;
    CORE.input.mouse.cursorHidden = false;
}

void DisableCursorPlatform(void)
{
    CORE.input.mouse.cursorLocked = true;
    CORE.input.mouse.cursorHidden = true;
}

void SetMouseCursorPlatform(int cursor)
{
    CORE.input.mouse.cursor = cursor;
    // No system cursor to change on a touch device
}

void SetMousePositionPlatform(int x, int y)
{
    CORE.input.mouse.currentPosition.x = (float)x;
    CORE.input.mouse.currentPosition.y = (float)y;
    CORE.input.mouse.previousPosition = CORE.input.mouse.currentPosition;
    // The physical touch position cannot be moved, only the reported one
}

//----------------------------------------------------------------------------------
// Platform contract: clipboard (text through JNI, no image clipboard on Android)
//----------------------------------------------------------------------------------
void SetClipboardTextPlatform(const char *text)
{
    if (text == NULL) return;

    bool attached = false;
    JNIEnv *env = AttachJni(&attached);
    if (env == NULL) return;

    jobject manager = GetClipboardManager(env);
    jclass clipDataClass = (*env)->FindClass(env, "android/content/ClipData");

    if ((manager == NULL) || (clipDataClass == NULL))
    {
        (*env)->ExceptionClear(env);
        TRACELOG(LOG_WARNING, "PLATFORM: Clipboard service not available");
        DetachJni(attached);
        return;
    }

    jmethodID newPlainText = (*env)->GetStaticMethodID(env, clipDataClass, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    jclass managerClass = (*env)->GetObjectClass(env, manager);
    jmethodID setPrimaryClip = (managerClass != NULL)?
        (*env)->GetMethodID(env, managerClass, "setPrimaryClip", "(Landroid/content/ClipData;)V") : NULL;

    if ((newPlainText == NULL) || (setPrimaryClip == NULL))
    {
        (*env)->ExceptionClear(env);
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to resolve clipboard methods");
        DetachJni(attached);
        return;
    }

    jstring label = (*env)->NewStringUTF(env, "rayvulkan");
    jstring value = (*env)->NewStringUTF(env, text);
    jobject clip = (*env)->CallStaticObjectMethod(env, clipDataClass, newPlainText, label, value);

    if (clip != NULL) (*env)->CallVoidMethod(env, manager, setPrimaryClip, clip);
    else TRACELOG(LOG_WARNING, "PLATFORM: Failed to build clipboard data");

    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionClear(env);
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to write clipboard text");
    }

    DetachJni(attached);
}

const char *GetClipboardTextPlatform(void)
{
    platform.clipboardBuffer[0] = '\0';

    bool attached = false;
    JNIEnv *env = AttachJni(&attached);
    if (env == NULL) return platform.clipboardBuffer;

    jobject manager = GetClipboardManager(env);
    if (manager == NULL)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Clipboard service not available");
        DetachJni(attached);
        return platform.clipboardBuffer;
    }

    jclass managerClass = (*env)->GetObjectClass(env, manager);
    jmethodID getPrimaryClip = (managerClass != NULL)?
        (*env)->GetMethodID(env, managerClass, "getPrimaryClip", "()Landroid/content/ClipData;") : NULL;
    jobject clip = (getPrimaryClip != NULL)? (*env)->CallObjectMethod(env, manager, getPrimaryClip) : NULL;

    if (clip == NULL)
    {
        (*env)->ExceptionClear(env);
        DetachJni(attached);
        return platform.clipboardBuffer;
    }

    jclass clipClass = (*env)->GetObjectClass(env, clip);
    jmethodID getItemAt = (*env)->GetMethodID(env, clipClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
    jobject item = (getItemAt != NULL)? (*env)->CallObjectMethod(env, clip, getItemAt, 0) : NULL;

    if (item != NULL)
    {
        jclass itemClass = (*env)->GetObjectClass(env, item);
        jmethodID coerceToText = (*env)->GetMethodID(env, itemClass, "coerceToText",
                                                     "(Landroid/content/Context;)Ljava/lang/CharSequence;");
        jobject sequence = (coerceToText != NULL)?
            (*env)->CallObjectMethod(env, item, coerceToText, platform.app->activity->clazz) : NULL;

        if (sequence != NULL)
        {
            jclass sequenceClass = (*env)->GetObjectClass(env, sequence);
            jmethodID toString = (*env)->GetMethodID(env, sequenceClass, "toString", "()Ljava/lang/String;");
            jstring string = (toString != NULL)? (jstring)(*env)->CallObjectMethod(env, sequence, toString) : NULL;

            if (string != NULL)
            {
                const char *utf8 = (*env)->GetStringUTFChars(env, string, NULL);
                if (utf8 != NULL)
                {
                    strncpy(platform.clipboardBuffer, utf8, sizeof(platform.clipboardBuffer) - 1);
                    platform.clipboardBuffer[sizeof(platform.clipboardBuffer) - 1] = '\0';
                    (*env)->ReleaseStringUTFChars(env, string, utf8);
                }
            }
        }
    }

    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionClear(env);
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to read clipboard text");
    }

    DetachJni(attached);

    return platform.clipboardBuffer;
}

Image GetClipboardImagePlatform(void)
{
    Image image = { 0 };

    TRACELOG(LOG_WARNING, "PLATFORM: Android does not provide an image clipboard");

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

    bool attached = false;
    JNIEnv *env = AttachJni(&attached);
    if (env == NULL) return;

    jclass uriClass = (*env)->FindClass(env, "android/net/Uri");
    jclass intentClass = (*env)->FindClass(env, "android/content/Intent");

    if ((uriClass == NULL) || (intentClass == NULL))
    {
        (*env)->ExceptionClear(env);
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to resolve Android intent classes");
        DetachJni(attached);
        return;
    }

    jmethodID parse = (*env)->GetStaticMethodID(env, uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jmethodID newIntent = (*env)->GetMethodID(env, intentClass, "<init>",
                                              "(Ljava/lang/String;Landroid/net/Uri;)V");
    jmethodID addFlags = (*env)->GetMethodID(env, intentClass, "addFlags", "(I)Landroid/content/Intent;");

    jclass activityClass = (*env)->GetObjectClass(env, platform.app->activity->clazz);
    jmethodID startActivity = (activityClass != NULL)?
        (*env)->GetMethodID(env, activityClass, "startActivity", "(Landroid/content/Intent;)V") : NULL;

    if ((parse == NULL) || (newIntent == NULL) || (startActivity == NULL))
    {
        (*env)->ExceptionClear(env);
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to resolve Android intent methods");
        DetachJni(attached);
        return;
    }

    jstring urlString = (*env)->NewStringUTF(env, url);
    jobject uri = (*env)->CallStaticObjectMethod(env, uriClass, parse, urlString);
    jstring action = (*env)->NewStringUTF(env, "android.intent.action.VIEW");
    jobject intent = (*env)->NewObject(env, intentClass, newIntent, action, uri);

    if (intent != NULL)
    {
        if (addFlags != NULL) (*env)->CallObjectMethod(env, intent, addFlags, 0x10000000); // FLAG_ACTIVITY_NEW_TASK
        (*env)->CallVoidMethod(env, platform.app->activity->clazz, startActivity, intent);
    }

    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionClear(env);
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to open URL");
    }

    DetachJni(attached);
}

void SetGamepadVibrationPlatform(int gamepad, float leftMotor, float rightMotor, float duration)
{
    (void)gamepad;
    (void)leftMotor;
    (void)rightMotor;
    (void)duration;

    // Rumble needs InputDevice.getVibrator() through JNI plus the VIBRATE permission
    TRACELOG(LOG_WARNING, "PLATFORM: Gamepad vibration is not supported on the Android backend");
}

int SetGamepadMappingsPlatform(const char *mappings)
{
    (void)mappings;

    // Android normalizes gamepad input itself, custom mappings do not apply
    TRACELOG(LOG_WARNING, "PLATFORM: Custom gamepad mappings are not supported on Android");

    return 0;
}
