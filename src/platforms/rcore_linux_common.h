/**********************************************************************************************
*
*   rcore_linux_common - Shared helpers for the Linux platform backends (X11 / Wayland)
*
*   Provides, as static inline helpers so that exactly one backend .c includes it:
*     - Monotonic timing (clock_gettime) used by GetTimePlatform()/WaitTimePlatform()
*     - Linux joystick API (/dev/input/js*) gamepad support with hotplug rescan,
*       axis/button mapping to the raylib gamepad enums and evdev force-feedback rumble
*
*   CONTRACT NOTE (extension of src/rcore_internal.h): nothing here is part of the
*   platform contract; the backends implement the Platform* functions as thin wrappers.
*
*   REQUIREMENT: the including .c must define _GNU_SOURCE (or _POSIX_C_SOURCE >= 200809L)
*   before any system header so clock_gettime()/nanosleep() are visible under -std=c99.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#ifndef RCORE_LINUX_COMMON_H
#define RCORE_LINUX_COMMON_H

#include "raylib.h"
#include "rcore_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

// rcore_linux_common.h pulls in <linux/input.h> for the evdev/joystick gamepad code, which
// defines 500+ object-like `KEY_*` macros (KEY_SPACE 57, KEY_A 30, ...). Those macros shadow
// raylib's KeyboardKey enumerators of the same name, so every `KEY_*` written below would mean
// an evdev scancode instead of the raylib key an application compares against. Drop the macros;
// the enumerators from raylib.h remain visible and the gamepad code only needs BTN_*/ABS_*.
#undef KEY_A
#undef KEY_APOSTROPHE
#undef KEY_B
#undef KEY_BACK
#undef KEY_BACKSLASH
#undef KEY_BACKSPACE
#undef KEY_C
#undef KEY_COMMA
#undef KEY_D
#undef KEY_DELETE
#undef KEY_DOWN
#undef KEY_E
#undef KEY_END
#undef KEY_ENTER
#undef KEY_EQUAL
#undef KEY_F
#undef KEY_F1
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_G
#undef KEY_GRAVE
#undef KEY_H
#undef KEY_HOME
#undef KEY_I
#undef KEY_INSERT
#undef KEY_J
#undef KEY_K
#undef KEY_L
#undef KEY_LEFT
#undef KEY_M
#undef KEY_MENU
#undef KEY_MINUS
#undef KEY_N
#undef KEY_O
#undef KEY_P
#undef KEY_PAUSE
#undef KEY_Q
#undef KEY_R
#undef KEY_RIGHT
#undef KEY_S
#undef KEY_SEMICOLON
#undef KEY_SLASH
#undef KEY_SPACE
#undef KEY_T
#undef KEY_TAB
#undef KEY_U
#undef KEY_UP
#undef KEY_V
#undef KEY_W
#undef KEY_X
#undef KEY_Y
#undef KEY_Z

#include <linux/joystick.h>

//----------------------------------------------------------------------------------
// Timing
//----------------------------------------------------------------------------------

// Monotonic clock in seconds; never goes backwards
static inline double rlLinuxGetTime(void)
{
    struct timespec ts = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec*1.0e-9;
}

// Sleep for the given amount of seconds; values <= 0 return immediately
static inline void rlLinuxWaitTime(double seconds)
{
    if (seconds <= 0.0) return;

    struct timespec req = { 0 };
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec)*1.0e9);
    if (req.tv_nsec < 0) req.tv_nsec = 0;
    if (req.tv_nsec > 999999999L) req.tv_nsec = 999999999L;

    struct timespec rem = { 0 };
    while (nanosleep(&req, &rem) == -1)
    {
        if (errno != EINTR) break;
        req = rem;
    }
}

//----------------------------------------------------------------------------------
// Gamepads: Linux joystick API (/dev/input/js*)
//----------------------------------------------------------------------------------

#define RL_LINUX_GAMEPAD_RESCAN_TIME   2.0f    // Seconds between hotplug rescans

typedef struct rlLinuxGamepad {
    int fd;                 // /dev/input/jsN descriptor, -1 when unplugged
    int index;              // N in /dev/input/jsN
    int ffFd;               // /dev/input/eventM descriptor for rumble, -1 when unavailable
    int ffEffectId;         // Uploaded rumble effect id, -1 when none
} rlLinuxGamepad;

typedef struct rlLinuxGamepadState {
    rlLinuxGamepad pads[MAX_GAMEPADS];
    double lastScanTime;
    bool mappingsWarned;
} rlLinuxGamepadState;

static rlLinuxGamepadState rlLinuxPads;

// Map a Linux joystick button index to a raylib GamepadButton (xpad/standard layout)
static inline int rlLinuxMapGamepadButton(int button)
{
    static const int map[] = {
        GAMEPAD_BUTTON_RIGHT_FACE_DOWN,     // BTN_A / cross
        GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,    // BTN_B / circle
        GAMEPAD_BUTTON_RIGHT_FACE_LEFT,     // BTN_X / square
        GAMEPAD_BUTTON_RIGHT_FACE_UP,       // BTN_Y / triangle
        GAMEPAD_BUTTON_LEFT_TRIGGER_1,      // BTN_TL
        GAMEPAD_BUTTON_RIGHT_TRIGGER_1,     // BTN_TR
        GAMEPAD_BUTTON_MIDDLE_LEFT,         // BTN_SELECT
        GAMEPAD_BUTTON_MIDDLE_RIGHT,        // BTN_START
        GAMEPAD_BUTTON_MIDDLE,              // BTN_MODE / guide
        GAMEPAD_BUTTON_LEFT_THUMB,          // BTN_THUMBL
        GAMEPAD_BUTTON_RIGHT_THUMB          // BTN_THUMBR
    };

    if ((button >= 0) && (button < (int)(sizeof(map)/sizeof(map[0])))) return map[button];
    if ((button > 0) && (button < MAX_GAMEPAD_BUTTONS)) return button;   // Passthrough for exotic pads
    return -1;
}

// Map a Linux joystick axis index to a raylib GamepadAxis, or -1 when it is a d-pad axis
static inline int rlLinuxMapGamepadAxis(int axis)
{
    switch (axis)
    {
        case 0: return GAMEPAD_AXIS_LEFT_X;
        case 1: return GAMEPAD_AXIS_LEFT_Y;
        case 2: return GAMEPAD_AXIS_LEFT_TRIGGER;
        case 3: return GAMEPAD_AXIS_RIGHT_X;
        case 4: return GAMEPAD_AXIS_RIGHT_Y;
        case 5: return GAMEPAD_AXIS_RIGHT_TRIGGER;
        default: break;
    }
    return -1;
}

// Set the four d-pad buttons of a gamepad from a hat axis value
static inline void rlLinuxSetGamepadHat(int gamepad, int axis, int value)
{
    int negative = (axis == 6)? GAMEPAD_BUTTON_LEFT_FACE_LEFT : GAMEPAD_BUTTON_LEFT_FACE_UP;
    int positive = (axis == 6)? GAMEPAD_BUTTON_LEFT_FACE_RIGHT : GAMEPAD_BUTTON_LEFT_FACE_DOWN;

    CORE.input.gamepad.currentButtonState[gamepad][negative] = (value < -16384)? 1 : 0;
    CORE.input.gamepad.currentButtonState[gamepad][positive] = (value > 16384)? 1 : 0;
    if (value < -16384) CORE.input.gamepad.lastButtonPressed = negative;
    else if (value > 16384) CORE.input.gamepad.lastButtonPressed = positive;
}

// Find and open the evdev node of /dev/input/jsN that supports force feedback rumble
// Returns -1 when the device has no usable force-feedback node
static inline int rlLinuxOpenForceFeedback(int jsIndex)
{
    char pattern[128] = { 0 };
    snprintf(pattern, sizeof(pattern), "/sys/class/input/js%i/device/event*", jsIndex);

    glob_t found = { 0 };
    if (glob(pattern, 0, NULL, &found) != 0)
    {
        globfree(&found);
        return -1;
    }

    int fd = -1;
    for (size_t i = 0; (i < found.gl_pathc) && (fd < 0); i++)
    {
        const char *name = strrchr(found.gl_pathv[i], '/');
        if (name == NULL) continue;

        char device[128] = { 0 };
        snprintf(device, sizeof(device), "/dev/input%s", name);

        int candidate = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (candidate < 0) continue;

        unsigned long features[(FF_MAX/(8*sizeof(unsigned long))) + 1] = { 0 };
        if (ioctl(candidate, EVIOCGBIT(EV_FF, sizeof(features)), features) >= 0)
        {
            size_t word = FF_RUMBLE/(8*sizeof(unsigned long));
            size_t bit = FF_RUMBLE%(8*sizeof(unsigned long));
            if ((features[word] & (1UL << bit)) != 0) fd = candidate;
        }

        if (candidate != fd) close(candidate);
    }

    globfree(&found);
    return fd;
}

// Open /dev/input/jsN into the given raylib gamepad slot; returns true on success
static inline bool rlLinuxOpenGamepad(int gamepad, int jsIndex)
{
    char device[32] = { 0 };
    snprintf(device, sizeof(device), "/dev/input/js%i", jsIndex);

    int fd = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;

    char name[64] = { 0 };
    if (ioctl(fd, JSIOCGNAME(sizeof(name) - 1), name) < 0) strcpy(name, "Unknown Gamepad");

    unsigned char axes = 0, buttons = 0;
    ioctl(fd, JSIOCGAXES, &axes);
    ioctl(fd, JSIOCGBUTTONS, &buttons);

    rlLinuxPads.pads[gamepad].fd = fd;
    rlLinuxPads.pads[gamepad].index = jsIndex;
    rlLinuxPads.pads[gamepad].ffFd = rlLinuxOpenForceFeedback(jsIndex);
    rlLinuxPads.pads[gamepad].ffEffectId = -1;

    CORE.input.gamepad.ready[gamepad] = true;
    CORE.input.gamepad.axisCount[gamepad] = (axes > MAX_GAMEPAD_AXES)? MAX_GAMEPAD_AXES : (int)axes;
    memset(CORE.input.gamepad.axisState[gamepad], 0, sizeof(CORE.input.gamepad.axisState[gamepad]));
    memset(CORE.input.gamepad.currentButtonState[gamepad], 0, sizeof(CORE.input.gamepad.currentButtonState[gamepad]));
    memset(CORE.input.gamepad.previousButtonState[gamepad], 0, sizeof(CORE.input.gamepad.previousButtonState[gamepad]));
    strncpy(CORE.input.gamepad.name[gamepad], name, sizeof(CORE.input.gamepad.name[gamepad]) - 1);
    CORE.input.gamepad.name[gamepad][sizeof(CORE.input.gamepad.name[gamepad]) - 1] = '\0';

    // Triggers rest at -1.0 in the raylib convention
    if (CORE.input.gamepad.axisCount[gamepad] > GAMEPAD_AXIS_LEFT_TRIGGER) CORE.input.gamepad.axisState[gamepad][GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
    if (CORE.input.gamepad.axisCount[gamepad] > GAMEPAD_AXIS_RIGHT_TRIGGER) CORE.input.gamepad.axisState[gamepad][GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;

    TRACELOG(LOG_INFO, "PLATFORM: Gamepad %i connected: %s (%i axes, %i buttons)", gamepad, name, (int)axes, (int)buttons);
    return true;
}

// Release a gamepad slot and mark it as not ready
static inline void rlLinuxCloseGamepad(int gamepad)
{
    if (rlLinuxPads.pads[gamepad].fd >= 0) close(rlLinuxPads.pads[gamepad].fd);
    if (rlLinuxPads.pads[gamepad].ffFd >= 0) close(rlLinuxPads.pads[gamepad].ffFd);

    rlLinuxPads.pads[gamepad].fd = -1;
    rlLinuxPads.pads[gamepad].ffFd = -1;
    rlLinuxPads.pads[gamepad].ffEffectId = -1;
    rlLinuxPads.pads[gamepad].index = -1;

    CORE.input.gamepad.ready[gamepad] = false;
    CORE.input.gamepad.axisCount[gamepad] = 0;
    CORE.input.gamepad.name[gamepad][0] = '\0';
    memset(CORE.input.gamepad.currentButtonState[gamepad], 0, sizeof(CORE.input.gamepad.currentButtonState[gamepad]));
    memset(CORE.input.gamepad.axisState[gamepad], 0, sizeof(CORE.input.gamepad.axisState[gamepad]));
}

// True when the /dev/input/jsN device is already assigned to a raylib gamepad slot
static inline bool rlLinuxGamepadIsOpen(int jsIndex)
{
    for (int i = 0; i < MAX_GAMEPADS; i++)
    {
        if ((rlLinuxPads.pads[i].fd >= 0) && (rlLinuxPads.pads[i].index == jsIndex)) return true;
    }
    return false;
}

// Scan /dev/input/js0..js(MAX) and bind newly appeared devices to free slots
static inline void rlLinuxScanGamepads(void)
{
    for (int js = 0; js < 8; js++)
    {
        if (rlLinuxGamepadIsOpen(js)) continue;

        char device[32] = { 0 };
        snprintf(device, sizeof(device), "/dev/input/js%i", js);
        if (access(device, R_OK) != 0) continue;

        for (int i = 0; i < MAX_GAMEPADS; i++)
        {
            if (rlLinuxPads.pads[i].fd >= 0) continue;
            if (rlLinuxOpenGamepad(i, js)) break;
        }
    }
}

// Initialize gamepad support: clear slots and do the first device scan
static inline void rlLinuxInitGamepads(void)
{
    for (int i = 0; i < MAX_GAMEPADS; i++)
    {
        rlLinuxPads.pads[i].fd = -1;
        rlLinuxPads.pads[i].ffFd = -1;
        rlLinuxPads.pads[i].ffEffectId = -1;
        rlLinuxPads.pads[i].index = -1;
    }
    rlLinuxPads.lastScanTime = 0.0;
    rlLinuxScanGamepads();
    rlLinuxPads.lastScanTime = rlLinuxGetTime();
}

// Close every open gamepad; safe to call when none were opened
static inline void rlLinuxCloseGamepads(void)
{
    for (int i = 0; i < MAX_GAMEPADS; i++)
    {
        if (rlLinuxPads.pads[i].fd >= 0) rlLinuxCloseGamepad(i);
    }
}

// Drain pending joystick events into CORE.input.gamepad; call once per frame
static inline void rlLinuxPollGamepads(void)
{
    double now = rlLinuxGetTime();
    if ((now - rlLinuxPads.lastScanTime) >= RL_LINUX_GAMEPAD_RESCAN_TIME)
    {
        rlLinuxPads.lastScanTime = now;
        rlLinuxScanGamepads();
    }

    for (int i = 0; i < MAX_GAMEPADS; i++)
    {
        if (rlLinuxPads.pads[i].fd < 0) continue;

        memcpy(CORE.input.gamepad.previousButtonState[i], CORE.input.gamepad.currentButtonState[i],
               sizeof(CORE.input.gamepad.currentButtonState[i]));

        struct js_event event = { 0 };
        for (;;)
        {
            ssize_t got = read(rlLinuxPads.pads[i].fd, &event, sizeof(event));
            if (got != (ssize_t)sizeof(event))
            {
                if ((got < 0) && (errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR)) rlLinuxCloseGamepad(i);
                break;
            }

            unsigned char type = (unsigned char)(event.type & ~JS_EVENT_INIT);

            if (type == JS_EVENT_BUTTON)
            {
                int button = rlLinuxMapGamepadButton((int)event.number);
                if ((button < 0) || (button >= MAX_GAMEPAD_BUTTONS)) continue;

                CORE.input.gamepad.currentButtonState[i][button] = (event.value != 0)? 1 : 0;
                if (event.value != 0) CORE.input.gamepad.lastButtonPressed = button;
            }
            else if (type == JS_EVENT_AXIS)
            {
                if ((event.number == 6) || (event.number == 7))
                {
                    rlLinuxSetGamepadHat(i, (int)event.number, (int)event.value);
                    continue;
                }

                int axis = rlLinuxMapGamepadAxis((int)event.number);
                if ((axis < 0) || (axis >= MAX_GAMEPAD_AXES)) continue;

                float value = (float)event.value/32767.0f;
                if (value < -1.0f) value = -1.0f;
                else if (value > 1.0f) value = 1.0f;
                CORE.input.gamepad.axisState[i][axis] = value;

                // Triggers additionally drive their digital buttons, as raylib does
                if (axis == GAMEPAD_AXIS_LEFT_TRIGGER) CORE.input.gamepad.currentButtonState[i][GAMEPAD_BUTTON_LEFT_TRIGGER_2] = (value > -0.5f)? 1 : 0;
                else if (axis == GAMEPAD_AXIS_RIGHT_TRIGGER) CORE.input.gamepad.currentButtonState[i][GAMEPAD_BUTTON_RIGHT_TRIGGER_2] = (value > -0.5f)? 1 : 0;
            }
        }
    }
}

// Play a rumble effect on a gamepad via evdev force feedback; no-op when unsupported
// gamepad in [0, MAX_GAMEPADS), motors in [0, 1], duration in seconds
static inline void rlLinuxSetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration)
{
    if ((gamepad < 0) || (gamepad >= MAX_GAMEPADS) || !CORE.input.gamepad.ready[gamepad]) return;
    if (rlLinuxPads.pads[gamepad].ffFd < 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Gamepad %i does not support force feedback rumble", gamepad);
        return;
    }
    if (duration <= 0.0f) return;

    if (leftMotor < 0.0f) leftMotor = 0.0f;
    else if (leftMotor > 1.0f) leftMotor = 1.0f;
    if (rightMotor < 0.0f) rightMotor = 0.0f;
    else if (rightMotor > 1.0f) rightMotor = 1.0f;
    if (duration > 60.0f) duration = 60.0f;

    struct ff_effect effect = { 0 };
    effect.type = FF_RUMBLE;
    effect.id = rlLinuxPads.pads[gamepad].ffEffectId;    // -1 uploads a new effect
    effect.replay.length = (unsigned short)(duration*1000.0f);
    effect.replay.delay = 0;
    effect.u.rumble.strong_magnitude = (unsigned short)(leftMotor*65535.0f);
    effect.u.rumble.weak_magnitude = (unsigned short)(rightMotor*65535.0f);

    if (ioctl(rlLinuxPads.pads[gamepad].ffFd, EVIOCSFF, &effect) < 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to upload rumble effect for gamepad %i", gamepad);
        return;
    }
    rlLinuxPads.pads[gamepad].ffEffectId = effect.id;

    struct input_event play = { 0 };
    play.type = EV_FF;
    play.code = (unsigned short)effect.id;
    play.value = 1;
    if (write(rlLinuxPads.pads[gamepad].ffFd, &play, sizeof(play)) != (ssize_t)sizeof(play))
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to play rumble effect for gamepad %i", gamepad);
    }
}

// Accept SDL-style gamepad mappings; the Linux joystick backend uses a fixed mapping
static inline int rlLinuxSetGamepadMappings(const char *mappings)
{
    if (mappings == NULL) return 0;
    if (!rlLinuxPads.mappingsWarned)
    {
        rlLinuxPads.mappingsWarned = true;
        TRACELOG(LOG_WARNING, "PLATFORM: SetGamepadMappings() is not supported, the Linux joystick backend uses a fixed mapping");
    }
    return 0;
}

#endif // RCORE_LINUX_COMMON_H
