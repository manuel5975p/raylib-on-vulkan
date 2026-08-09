/**********************************************************************************************
*
*   fix_win32_compatibility - Include <windows.h> in a translation unit that also uses raylib.h
*
*   The Win32 headers export several identifiers that collide with the raylib public API:
*
*     wingdi.h : BOOL Rectangle(HDC, int, int, int, int)   vs  struct Rectangle
*     winuser.h: BOOL CloseWindow(HWND)                    vs  void CloseWindow(void)
*     winuser.h: int  ShowCursor(BOOL)                     vs  void ShowCursor(void)
*     winuser.h: LoadImage / DrawText / DrawTextEx (macros) vs raylib functions
*     mmsystem.h: PlaySound (macro)                        vs  raylib PlaySound
*
*   The three *functions* are renamed before <windows.h> is parsed, which changes the
*   declared (and therefore linked) symbol name — so raylib-on-vulkan must never call them.
*   Equivalents that are used instead:
*     Rectangle()   -> not needed (no GDI drawing)
*     CloseWindow() -> ShowWindow(hwnd, SW_MINIMIZE)
*     ShowCursor()  -> SetCursor(NULL) from WM_SETCURSOR
*
*   The *macros* are simply #undef'd afterwards, they cost nothing.
*
*   NOTE: WIN32_LEAN_AND_MEAN excludes shellapi.h/timeapi.h/mmsystem.h, include those
*   explicitly after this header when needed.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#ifndef FIX_WIN32_COMPATIBILITY_H
#define FIX_WIN32_COMPATIBILITY_H

#if !defined(_WIN32)
#   error "fix_win32_compatibility.h is only usable on Windows"
#endif

#if defined(_WINDOWS_)
#   error "<windows.h> was included before fix_win32_compatibility.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#   define NOMINMAX
#endif

#define Rectangle   Win32Rectangle_unused
#define CloseWindow Win32CloseWindow_unused
#define ShowCursor  Win32ShowCursor_unused

#include <windows.h>

#undef Rectangle
#undef CloseWindow
#undef ShowCursor

#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef PlaySound
#undef near
#undef far

#endif // FIX_WIN32_COMPATIBILITY_H
