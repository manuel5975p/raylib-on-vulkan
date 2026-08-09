# raylib-on-vulkan

A from-scratch reimplementation of the full [raylib](https://www.raylib.com) API
(see `api.md`) on **pure Vulkan**, with hand-written platform window managers -
no GLFW, no SDL, no OpenGL.

- **Renderer**: `src/rlvk.c` - an rlgl-compatible immediate-mode batch renderer
  on Vulkan 1.2+, loaded exclusively through [volk](https://github.com/zeux/volk).
  Lazy render passes, pipeline cache keyed on state, SPIR-V reflection for user
  shader uniforms, render textures, instancing, readbacks.
- **Platforms** (`src/platforms/`), each written directly against the native API:
  - `rcore_x11.c` - raw Xlib: EWMH, XRandR, Xcursor, ICCCM clipboard (+INCR), Xdnd v5
  - `rcore_wayland.c` - raw libwayland-client: xdg-shell, server-side decorations,
    pointer-constraints/relative-pointer, wl_data_device clipboard & drops
  - `rcore_win32.c` - raw Win32: WndProc, raw input, XInput gamepads (runtime-loaded)
  - `rcore_android.c` - hand-written NativeActivity glue (two-thread model, AInputQueue)
  - `rcore_macos.c` - pure C via the Objective-C runtime (NSWindow/NSEvent/CAMetalLayer,
    MoltenVK portability)
- **Modules**: `rcore`, `rshapes`, `rtextures`, `rtext`, `rmodels`, `raudio` -
  full api.md coverage, raylib-parity semantics, C99.
- **Audio**: miniaudio device + raylib-style mixer; wav/ogg/mp3/flac/qoa/xm/mod.
- **Custom Vulkan interop** (`src/rayvk.h`): record and submit your own Vulkan
  commands alongside raylib drawing - `GetVkDevice()`, `GetVkCurrentCommandBuffer()`
  (batch-flushed, in-pass), `BeginVkCustomMode()/EndVkCustomMode()` (render pass
  suspended), `BeginVkCommands()/EndVkCommands()/SubmitVkCommands()` (one-shot).
  See `examples/others/vk_custom_commands.c`.

## Building

```sh
cmake -S . -B build -G Ninja          # X11 backend by default on Linux
cmake -S . -B build -G Ninja -DPLATFORM=WAYLAND   # native Wayland
ninja -C build
ctest --test-dir build --timeout 60   # BUILD_TESTING=ON by default
```

Or with plain GNU Make, mirroring upstream raylib:

```sh
make -C src                           # librayvulkan.a in src/
make -C examples PLATFORM=WAYLAND     # binaries beside their sources
make -C tests test                    # run the test suite
```

`PLATFORM` ∈ `X11 | WAYLAND | WIN32 | ANDROID | MACOS` (auto-detected;
`RAYVK_PLATFORM` is still accepted as a CMake alias).
Requires: CMake ≥ 3.22, ninja, a C99 compiler, `glslc` (shaderc) for the
embedded default shaders, Vulkan driver at runtime. Vulkan headers are vendored.

## Shaders

This backend consumes **SPIR-V**, not GLSL, at runtime. Compile shaders offline:

```sh
glslc -O -g myshader.frag -o myshader.frag.spv   # -g keeps names for GetShaderLocation()
```

`LoadShader("v.spv", "f.spv")` + the shader ABI documented in `src/rlvk.h`
(fixed vertex input locations, push-constant `mvp`/`matModel`, set 0 = uniform
block, set 1 = `texture0..3`).

## Examples

`examples/` mirrors upstream raylib's categorized examples tree
(`core`, `shapes`, `textures`, `text`, `models`, `shaders`, `audio`, `others`):
the full upstream suite ported to Vulkan (216 examples passing, per-category
`STATUS.md` reports) plus raylib-on-vulkan-specific additions such as
`others/vk_custom_commands.c`. Binaries build to
`build/examples/<category>/` next to a copied `resources/` dir - run them from
that directory. GLSL shaders are ported to the Vulkan ABI under
`<category>/resources/shaders/vk/` and compiled to SPIR-V at build time.
Excluded (inherently OpenGL/GLFW/web-bound): `rlgl_standalone`,
`raylib_opengl_interop`, `shaders_rlgl_compute`, `core_*_web`.
See `examples/PORTING.md` for the porting rules and shader ABI notes.

## Layout

```
api.md          the API contract (raylib)
CMakeOptions.txt  build options (platform backend, examples, tests)
cmake/          CMake helper modules
src/            the library (own CMakeLists.txt + Makefile, like upstream)
src/raylib.h    public header
src/rayvk.h     public Vulkan interop header
src/rlvk.h      internal renderer contract (rlgl-compatible)
src/rcore_internal.h  internal platform backend contract
src/platforms/  one window manager per platform
src/external/   vendored third-party single-header libs + volk + Vulkan headers
examples/       categorized examples (upstream raylib layout)
tests/          ctest suite (unit + windowed render verification)
tools/          build-time helpers (SPIR-V embedding)
```

License: zlib/libpng, same as raylib. Vendored libraries keep their own licenses.
