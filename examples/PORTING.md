# Porting upstream raylib examples to raylib-on-vulkan

## Ground rules
- Keep diffs to example .c files MINIMAL — these are upstream sources; only touch
  what the Vulkan backend requires (shader file paths, rlgl notes). Never restyle.
- `#include "rlgl.h"` works (compat shim over rlvk.h). The rl* API is in src/rlvk.h;
  if an example uses an rl* function that does not exist there, adapt the example
  (prefer) or report the gap — do NOT add to rlvk.h yourself.
- Excluded examples are listed in CMakeLists.txt with reasons; extend the list only
  when something is fundamentally OpenGL/web-bound, always with a reason comment.

## Shaders
Runtime shaders must be SPIR-V. For an example loading
`resources/shaders/glsl330/foo.fs`:
1. Write Vulkan GLSL ports in `<category>/resources/shaders/vk/foo.frag`
   (and `foo.vert` when the example has a custom VS; use extensions .vert/.frag/.comp).
2. The build compiles them to `resources/shaders/vk/foo.frag.spv` in the build tree.
3. Change the example to load the .spv paths, e.g.
   `LoadShader(0, TextFormat("resources/shaders/vk/foo.frag.spv"))`.
   (Drop the GLSL_VERSION TextFormat indirection; keep the variable if it keeps the
   diff smaller.)

Shader ABI (see src/rlvk.h header): vertex inputs at fixed locations
(0 pos vec3, 1 texcoord vec2, 2 normal vec3, 3 color vec4, 4 tangent, 5 texcoord2,
6 boneIds uvec4, 7 boneWeights vec4, 8-11 instanceTransform mat4);
`layout(push_constant) uniform PC { mat4 mvp; mat4 matModel; }`;
ALL other uniforms in ONE block: `layout(set = 0, binding = 0) uniform UBO { ... };`
— keep raylib's uniform names as MEMBER names (GetShaderLocation resolves members);
samplers: `layout(set = 1, binding = N) uniform sampler2D texture0..3;` (N = 0..3).
Varyings between VS/FS need matching explicit `layout(location = N)`.
Default VS outputs: location 0 = vec2 fragTexCoord, 1 = vec4 fragColor
(a custom FS paired with the default VS must consume exactly those).
raylib GLSL convention names map 1:1: vertexPosition/vertexTexCoord/vertexNormal/
vertexColor stay identical; gl_FragCoord works; `texture2D()`->`texture()`,
`varying`->in/out. Y direction: gl_FragCoord has top-left origin on the swapchain
target (GL had bottom-left) — port screen-space effects with care and verify visually.

## Building
From repo root: cmake reconfigure is needed after ADDING shader files (globs):
  cmake -S . -B <builddir> -G Ninja -DRAYVK_PLATFORM=WAYLAND && ninja -C <builddir> <example_name>
Binaries land in <builddir>/examples-upstream/<category>/ next to a copied
resources/ dir — ALWAYS run them with that directory as CWD.

## Headless verification protocol (do not open windows on the user session)
Start one private compositor per agent (unique socket!):
  kwin_wayland --virtual --no-lockscreen --socket rayvk-<category> \
      >/dev/null 2>&1 &
Then for each example:
  cd <builddir>/examples-upstream/<category>
  WAYLAND_DISPLAY=rayvk-<category> timeout 8 ./<name> >log.txt 2>&1
PASS criteria: log contains "RLVK: Vulkan renderer initialized successfully"
AND (exit code 124 [still running when killed] or clean 0 AFTER the loop —
audio/music examples auto-run; interactive ones just idle, that is fine).
Any crash (segv/abort), Vulkan ERROR log line, or init failure = FAIL to fix.
Kill your compositor when done — ONLY by its socket name or saved PID:
  kwin_wayland --virtual ... --socket rayvk-<name> & KWIN_PID=$!
  ...
  kill $KWIN_PID          # or: pkill -f "socket rayvk-<name>"
NEVER run `pkill kwin_wayland` or `pkill -f kwin_wayland` — that pattern matches
the USER'S SESSION COMPOSITOR (/usr/bin/kwin_wayland --socket wayland-0) and
kills their entire desktop. Do not use DISPLAY=:1 or the user session socket
(wayland-0); never run examples on the user's visible desktop.
