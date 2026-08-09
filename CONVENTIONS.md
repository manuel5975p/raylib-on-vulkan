# raylib-on-vulkan implementation conventions (all contributors/agents MUST follow)

Goal: full raylib API (api.md == src/raylib.h) implemented on pure Vulkan via volk.
No OpenGL anywhere. Rendering goes through the rlgl-compatible layer in src/rlvk.h.

## Rules
- Pure C99, maximally portable across gcc/clang/msvc. No VLAs, no GNU extensions,
  no anonymous unions beyond what raylib.h already uses.
- Public API: exactly src/raylib.h (do not change signatures). Internal contracts:
  src/rlvk.h, src/rcore_internal.h, src/rayvk.h. If a contract is insufficient,
  extend it minimally and document the addition at the top of your module.
- Allocators: use RL_MALLOC/RL_CALLOC/RL_REALLOC/RL_FREE with the standard raylib
  fallback pattern (#ifndef RL_MALLOC ...) in each .c file.
- Logging: TRACELOG(LOG_INFO/WARNING/ERROR, "MODULE: ...") — prefix messages with
  module tag like raylib does (e.g. "TEXTURE:", "SHADER:", "PLATFORM:").
- Vendored third-party headers live in src/external/ (stb, dr_libs, miniaudio,
  cgltf, m3d, qoi, par_shapes, sdefl/sinfl, rprand, rltexgpu, volk...). Use them;
  do not add new dependencies. Vulkan access ONLY via volk (VK_NO_PROTOTYPES).
- Implementation-definition macros (STB_IMAGE_IMPLEMENTATION etc.) go in exactly
  one .c file; check other modules to avoid duplicate symbol clashes:
  rtextures.c owns stb_image*, qoi, rltexgpu; rtext.c owns stb_truetype,
  stb_rect_pack; rmodels.c owns cgltf, m3d, par_shapes, tinyobj, vox_loader;
  raudio.c owns miniaudio, dr_*, stb_vorbis, qoa, jar_*; rcore.c owns sdefl,
  sinfl, rprand, RAYMATH_IMPLEMENTATION, RCAMERA_IMPLEMENTATION,
  RGESTURES_IMPLEMENTATION; rlvk.c owns nothing (volk.c is compiled separately).
- Style: shallow control flow with early exits; factor complex conditions into
  static predicate functions; assertions for internal invariants; recoverable
  errors (TRACELOG warning + safe zeroed return) for user-triggered failures.
  Keep symbol visibility narrow: everything non-API is `static`.
- Headers self-contained; include module's own header first in its .c.
- Behavior reference is raylib 5.6-dev semantics (api.md). When raylib returns
  static buffers (TextFormat, GetDirectoryPath...), replicate that behavior.
- Shaders are SPIR-V at runtime (see rlvk.h header comment for the shader ABI).
- NEVER run git commands. Do not create build dirs other than ./build.
- Tests live in tests/, examples in examples/ (integration phase owns those).
