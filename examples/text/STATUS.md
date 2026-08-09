# text — porting status

16/16 examples ported, built and headless-verified: **16 PASS, 0 FAIL**.
(`raygui.h` is a header dependency, not an example.)

Verified per PORTING.md: private compositor (`--socket rayvk-misc`), 8 s timeout,
run from the build-tree category dir, audio routed to the `rayvk_null` PulseAudio
sink so nothing is audible. PASS = "RLVK: Vulkan renderer initialized successfully"
present, exit 124 (still running when killed), no ERROR / validation / crash lines,
and no non-benign WARNING (miniaudio device dumps and the Wayland
`SetMousePosition()` notice are ignored).

| example | result |
| --- | --- |
| text_3d_drawing | PASS |
| text_codepoints_loading | PASS |
| text_font_filters | PASS |
| text_font_loading | PASS |
| text_font_sdf | PASS |
| text_font_spritefont | PASS |
| text_format_text | PASS |
| text_inline_styling | PASS |
| text_input_box | PASS |
| text_rectangle_bounds | PASS |
| text_sprite_fonts | PASS |
| text_strings_management | PASS |
| text_unicode_emojis | PASS |
| text_unicode_ranges | PASS |
| text_words_alignment | PASS |
| text_writing_anim | PASS |

## Exclusions
None. See `exclude.cmake`.

## Porting deltas

Only two files differ from upstream, one line each.

**text_font_sdf.c** and **text_3d_drawing.c** — shader load path:

    LoadShader(0,    "resources/shaders/glsl%i/sdf.fs")           ->
    LoadShader("resources/shaders/vk/base.vert.spv", "resources/shaders/vk/sdf.frag.spv")
    LoadShader(NULL, "resources/shaders/glsl%i/alpha_discard.fs") ->
    LoadShader("resources/shaders/vk/base.vert.spv", "resources/shaders/vk/alpha_discard.frag.spv")

The explicit vertex shader is a **workaround for a library gap**, not a porting
requirement: `LoadShader(NULL, fs)` should reuse the built-in vertex shader (raylib
semantics, and the SPIR-V is already embedded as `default_vert_spv_data`), but
`rlLoadShaderCodeSize()` rejects a NULL `vsSpirv` outright
(src/rlvk.c, "SHADER: Invalid SPIR-V blobs provided"). Once that is fixed, the
`base.vert.spv` argument can be replaced by `0`/`NULL` and `base.vert` deleted.

`text_3d_drawing.c` needs no rlgl adaptation — every `rl*` call it makes
(rlBegin/rlEnd/rlVertex3f/rlTexCoord2f/rlNormal3f/rlColor4ub/rlPushMatrix/
rlPopMatrix/rlRotatef/rlTranslatef/rlSetTexture/rlCheckRenderBatchLimit) exists in
src/rlvk.h and works through the compat shim unchanged.

`GLSL_VERSION` is left defined but unused, to keep the diff to a single line per file.

## Shaders (`resources/shaders/vk/`)

- `sdf.frag` — port of glsl330/sdf.fs. `colDiffuse` dropped (unused upstream too);
  `texture0` at set 1, binding 0. `dFdx`/`dFdy`/`smoothstep` carry over unchanged.
- `alpha_discard.frag` — port of glsl330/alpha_discard.fs. `colDiffuse` kept as the
  sole set 0, binding 0 UBO member so `SHADER_LOC_COLOR_DIFFUSE` resolves.
- `base.vert` — verbatim copy of `src/shaders/default.vert` (see workaround above).

Neither fragment shader uses `gl_FragCoord`, so the swapchain Y-origin flip noted in
PORTING.md does not apply.

## Visual verification

`text_font_sdf` and `text_3d_drawing` were additionally captured with a temporary
`TakeScreenshot()` call (since reverted; kwin's virtual backend does not support the
wlr screencopy protocol that `grim` needs). Both match the upstream reference PNGs:
the SDF example renders its atlas and text upright, and the 3D example shows glyphs
with correct alpha discard (no opaque quad backgrounds) and correct orientation.
