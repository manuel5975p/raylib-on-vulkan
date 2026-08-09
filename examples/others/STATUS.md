# others — porting status

1/1 portable example built and headless-verified: **1 PASS, 0 FAIL**.
2 further examples remain excluded (globally, not by this category).

Verified per PORTING.md: private compositor (`--socket rayvk-misc`), 8 s timeout,
run from the build-tree category dir, `PULSE_SINK=rayvk_null` so nothing is audible.
PASS = "RLVK: Vulkan renderer initialized successfully" present, exit 124 (still
running when killed), no ERROR / validation / crash lines, no non-benign WARNING.

| example | result |
| --- | --- |
| embedded_files_loading | PASS |

`embedded_files_loading` needed **no changes at all**. It exercises
`LoadImageFromMemory()` / `LoadWaveFromMemory()` against the checked-in
`resources/image_data.h` and `resources/audio_data.h`; both paths work unmodified on
the Vulkan backend, and the audio device initialises normally.

## Exclusions

No per-category exclusions — `exclude.cmake` is empty. The two remaining `.c` files
here are already in the global `RAYVK_EXAMPLES_EXCLUDE` list in
`examples-upstream/CMakeLists.txt` and were left untouched as instructed:

- `raylib_opengl_interop` — GL compute/PBO interop; `examples/vk_custom_commands.c`
  is the Vulkan equivalent.
- `rlgl_standalone` — standalone rlgl + GLFW without a raylib window; it needs
  `rlLoadExtensions`, `rlVertex`, `rlRotate`, `rlTranslate`, none of which exist (or
  can exist) in `src/rlvk.h`.

`resources/shaders/glsl*/point_particle.{vs,fs}` belong exclusively to
`raylib_opengl_interop` and therefore have **no** Vulkan port; there is no
`resources/shaders/vk/` directory in this category. Should that example ever be
revived, its shaders would need porting then.

## Notes

Despite the category name, nothing here required an rlgl adaptation: the only
non-excluded example makes no `rl*` calls.
