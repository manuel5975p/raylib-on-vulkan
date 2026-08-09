# shaders examples — B half (names >= "shaders_m") port status

Build dir used: private Ninja build, `-DRAYVK_PLATFORM=WAYLAND -DBUILD_TESTING=OFF`.
Headless runs on a private kwin_wayland compositor (socket `rayvk-shadersB`), 8 s timeout.
PASS = "RLVK: Vulkan renderer initialized successfully" + exit 124/0 + zero `ERROR` /
`WARNING: SHADER` lines in the log. All PASS entries were additionally verified visually
via a temporary `TakeScreenshot()` frame hook (removed again afterwards).

## Results — 18 ported, 18 PASS, 1 excluded

| example | status | notes |
|---|---|---|
| shaders_mandelbrot_set | PASS | verified visually |
| shaders_mesh_instancing | PASS | rlights.h dropped, see below |
| shaders_model_shader | PASS | |
| shaders_multi_sample2d | PASS | second sampler (`texture1`) verified |
| shaders_normalmap_rendering | PASS | TBN mat3 varying at locations 4-6 |
| shaders_palette_switch | PASS | `ivec3 palette[8]` std140 array works |
| shaders_postprocessing | PASS | 12 fragment shaders; needed frame-scope fix |
| shaders_raymarching_rendering | PASS | gl_FragCoord Y flipped in shader |
| shaders_rounded_rectangle | PASS | gl_FragCoord Y flip removed from example |
| shaders_shadowmap_rendering | PASS | needed frame-scope + sampler-binding fix |
| shaders_shapes_textures | PASS | |
| shaders_simple_mask | PASS | spare material map moved to slot < 4 |
| shaders_spotlight_rendering | PASS | struct array flattened |
| shaders_texture_outline | PASS | |
| shaders_texture_rendering | PASS | |
| shaders_texture_tiling | PASS | |
| shaders_texture_waves | PASS | |
| shaders_vertex_displacement | PASS | vertex-stage sampler + UBO |
| shaders_rlgl_compute | EXCLUDED | no compute-shader/SSBO API in rlvk |

## Shaders added under `resources/shaders/vk/`

base.vert, bloom.frag, blur.frag, color_mix.frag, cross_hatching.frag,
cross_stitching.frag, cubes_panning.frag, dream_vision.frag, fisheye.frag,
grayscale.frag, lighting_instancing.vert, lighting_instancing.frag,
mandelbrot_set.frag, mask.frag, normalmap.vert, normalmap.frag, outline.frag,
palette_switch.frag, pixelizer.frag, posterization.frag, predator.frag,
raymarching.frag, rounded_rectangle.frag, scanlines.frag, shadowmap.vert,
shadowmap.frag, sobel.frag, spotlight.frag, tiling.frag, vertex_displacement.vert,
vertex_displacement.frag

## Porting deltas that were needed beyond the plain GLSL->SPIR-V translation

1. **`LoadShader(0, "…frag.spv")` does not work** (see library issues #1). Every
   fragment-only example now loads `resources/shaders/vk/base.vert.spv`, which is a
   byte-for-byte port of the default vertex shader.
2. **Struct-array uniforms are not reflected** (library issue #2). `lighting.fs`
   (`Light lights[4]`) and `spotlight.fs` (`Spot spots[3]`) were rewritten with flat
   parallel arrays / scalars:
   - `shaders_mesh_instancing` no longer includes `rlights.h`; it sets the single
     directional light through `lightEnabled/lightType/lightPosition/lightTarget/lightColor`
     and uses a dedicated `lighting_instancing.frag` (the shared `lighting.frag` is left to
     the A half untouched).
   - `shaders_spotlight_rendering` looks up `spotPos[i]`/`spotInner[i]`/`spotRadius[i]`.
3. **Render-texture passes moved inside `BeginDrawing()`/`EndDrawing()`** in
   `shaders_postprocessing` and `shaders_shadowmap_rendering` (library issue #3).
4. **Sampler binding by index, not GL texture unit**: `rlSetUniformSampler(loc, textureId)`
   replaces the `rlActiveTextureSlot(n) + rlEnableTexture(id) + rlSetUniform(loc, &n, INT)`
   idiom in `shaders_shadowmap_rendering` and `shaders_vertex_displacement`.
5. **`shaders_simple_mask`** uses `MATERIAL_MAP_NORMAL` (index 2) instead of
   `MATERIAL_MAP_EMISSION` (index 5) as the spare 2nd-texture slot (library issue #4).
6. **`shaders_shadowmap_rendering`**: `TRACELOG()` -> `TraceLog()` (the rlgl compat shim
   does not export the macro).
7. **gl_FragCoord origin**: `raymarching.frag` flips Y in the shader;
   `shaders_rounded_rectangle` drops the example-side Y flip (and negates the shadow
   offset), `rounded_rectangle.frag` flips the corner-radius Y test.
8. Uniform block members cannot have initializers in Vulkan GLSL: `pixelWidth`,
   `pixelHeight`, `invert`, `resolution` (sobel) became `const` — none of them is ever set
   by `shaders_postprocessing`.

## Library issues found (NOT fixed here — src/ untouched)

1. **`rlLoadShaderCodeSize()` rejects a NULL vertex shader** — `rlvk.c:2525`
   `if ((vsSpirv == NULL) || (fsSpirv == NULL) …) return 0;`. raylib's most common
   idiom `LoadShader(0, "foo.fs")` (documented in PORTING.md!) therefore always fails
   with `WARNING: SHADER: Invalid SPIR-V blobs provided`, and `BeginShaderMode()` on the
   resulting id==0 shader can segfault. It should fall back to the built-in default
   vertex module. Repro: `LoadShader(0, "resources/shaders/vk/grayscale.frag.spv")`.
   Worked around in all 13 fragment-only examples by loading `base.vert.spv`.

2. **SPIR-V reflection only walks top-level members of the set-0 UBO**
   (`rlvk.c:~1358`). Nested struct members are never named, and
   `rlGetLocationUniform()` drops everything after `[i]`, so `lights[0].enabled`,
   `lights[0].position`, `lights[0].color` all resolve to the *same* offset. Any
   raylib shader using `struct Light lights[N]` (i.e. `rlights.h`, the whole
   basic-lighting family, `pbr.fs`, `spotlight.fs`) is unusable as-is.

3. **Draw commands issued outside `BeginDrawing()`/`EndDrawing()` are silently
   dropped** — `rlvkBeginRenderPass()`/`rlvkFlushBatch()` early-out on
   `!RLVK.frameRecording`. raylib upstream permits `BeginTextureMode()` before
   `BeginDrawing()` and many examples do exactly that; in rlvk the render texture then
   keeps a never-initialized image (validation layer:
   `VUID-vkCmdDraw-None-09600 … expects VkImage … to be in layout
   SHADER_READ_ONLY_OPTIMAL — instead, current layout is UNDEFINED`) and samples as all
   zeroes. Repro: unmodified upstream `shaders_postprocessing.c` renders a blank screen.
   Either the frame should start lazily, or `BeginTextureMode()` outside a frame should
   warn loudly.

4. **`rlActiveTextureSlot(n)` silently no-ops for n >= 4** (`RLVK_MAX_TEXTURE_SLOTS`),
   after which `rlEnableTexture()` writes into whatever slot was active before —
   corrupting slot 0. This is hit by stock `rmodels.c:BindMaterialTextures()`, which
   calls `rlActiveTextureSlot(i)` with the *material map index* (up to 11). Any example
   using a material map >= 4 (e.g. `MATERIAL_MAP_EMISSION` == 5, as upstream
   `shaders_simple_mask` does) silently overwrites the diffuse texture binding.

5. **No compute API**: `rlLoadShader(code, RL_COMPUTE_SHADER)`,
   `rlLoadShaderProgramCompute()`, `rlLoadShaderBuffer()`, `rlBindShaderBuffer()`,
   `rlComputeShaderDispatch()`, `rlUpdateShaderBuffer()` do not exist in rlvk.h.
   `shaders_rlgl_compute` is excluded for this reason (a port would have to go through
   the `rayvk.h` interop API and is out of scope for a "minimal delta" port).

6. Minor: the rlgl compat shim does not provide the `TRACELOG()` macro that upstream
   examples including `rlgl.h` use.
