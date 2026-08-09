# shaders category - first half (names < "shaders_m") port status

Build dir: `build-ex-shadersA` (`-DRAYVK_PLATFORM=WAYLAND -DBUILD_TESTING=OFF`),
headless run on a private `kwin_wayland --virtual --socket rayvk-shadersA`.

PASS = "RLVK: Vulkan renderer initialized successfully" in the log, exit 124
(still running when killed), no `ERROR` and no `WARNING: SHADER` lines.

| # | Example | Result | Shaders ported (resources/shaders/vk/) |
|---|---------|--------|----------------------------------------|
| 1 | shaders_ascii_rendering    | PASS | ascii.frag |
| 2 | shaders_basic_lighting     | PASS | lighting.vert, lighting.frag |
| 3 | shaders_basic_pbr          | PASS | pbr.vert, pbr.frag |
| 4 | shaders_cel_shading        | PASS | cel.vert, cel.frag, outline_hull.vert, outline_hull.frag |
| 5 | shaders_color_correction   | PASS | color_correction.frag |
| 6 | shaders_custom_uniform     | PASS | swirl.frag |
| 7 | shaders_deferred_rendering | PASS | gbuffer.vert, gbuffer.frag, deferred_shading.vert, deferred_shading.frag |
| 8 | shaders_depth_rendering    | PASS | depth_render.frag |
| 9 | shaders_depth_writing      | PASS | depth_write.frag |
| 10 | shaders_eratosthenes_sieve | PASS | eratosthenes.frag |
| 11 | shaders_fog_rendering      | PASS | fog.vert, fog.frag |
| 12 | shaders_game_of_life       | PASS | game_of_life.frag |
| 13 | shaders_hot_reloading      | PASS | reload.frag |
| 14 | shaders_hybrid_rendering   | PASS | hybrid_raymarch.frag, hybrid_raster.frag |
| 15 | shaders_julia_set          | PASS | julia_set.frag |
| 16 | shaders_lightmap_rendering | PASS | lightmap.vert, lightmap.frag |

16/16 PASS, no exclusions (see the "A half" section of `exclude.cmake`).

## Porting conventions used

- `default.vert` is a copy of `src/shaders/default.vert` shipped in the example
  resources. Every `LoadShader(0, "...frag.spv")` had to become
  `LoadShader("resources/shaders/vk/default.vert.spv", "...frag.spv")` because
  rlvk rejects a NULL vertex blob (see "Library gaps" below).
- All non-sampler uniforms go into one `layout(std140, set = 0, binding = 0) uniform UBO`.
  When an example pairs a custom VS with a custom FS the block is declared
  **identically in both stages** - reflection merges set0/binding0 members by name
  across stages, so differing layouts would alias member offsets onto each other.
  (That is why `fog.vert` exists as a near-duplicate of `lighting.vert`: fog.frag
  needs extra members, so the shared block differs.)
- `struct Light lights[N]` arrays are flattened into parallel arrays
  `lightsEnabled/lightsType/lightsPosition/lightsTarget/lightsColor[/lightsIntensity]`,
  because `rlGetLocationUniform()` resolves plain UBO member names plus an
  optional `[i]` suffix; members *inside* a struct-array element are not
  addressable. `rlights.h` and the private light helper in `shaders_basic_pbr.c`
  were updated to ask for the flattened names. `rlights.h` is shared with the
  second half of the category (mesh_instancing etc.), its lighting shaders must
  use the same flattened names.
- `rlights.h`: `UpdateLightValues()` now widens the 1-byte `bool light.enabled`
  into an `int` before `SetShaderValue(..., SHADER_UNIFORM_INT)` (upstream read
  4 bytes out of a 1-byte bool).
- Screen-space effects: `gl_FragCoord` has a top-left origin on the Vulkan
  swapchain. `reload.frag` drops the `resolution.y - mouse.y` flip;
  `hybrid_raymarch.frag` flips y explicitly so the raymarched scene lines up
  with the rasterized one.

## Per-example porting notes

- **cel_shading**: `RL_CULL_FACE_FRONT/BACK` are not defined by rlvk; replaced by
  the documented `rlSetCullFace()` arguments 0 (front) / 1 (back).
- **depth_rendering / depth_writing / hybrid_rendering**: `TRACELOG()` is a raylib
  internal macro, replaced by the public `TraceLog()`. `uniform bool flipY` became
  an `int` (bools cannot be fed through `SetShaderValue`). Writing `gl_FragDepth`
  works unchanged.
- **lightmap_rendering**: the GL VAO plumbing
  (`rlEnableVertexArray`/`rlSetVertexAttribute`/`rlEnableVertexAttribute`) has no
  rlvk equivalent and is not needed - rlvk binds mesh attributes by `Mesh.vboId`
  slot. The texcoords2 buffer is now stored in `mesh.vboId[5]` (upstream stored it
  in `vboId[SHADER_LOC_VERTEX_TEXCOORD02]` == index 2, which is the normals slot).
- **deferred_rendering**:
  - `RL_PIXELFORMAT_*` / `RL_SHADER_UNIFORM_*` aliases do not exist, used the
    plain raylib enums. `PIXELFORMAT_UNCOMPRESSED_R16G16B16` maps to
    `VK_FORMAT_R16G16B16A16_SFLOAT`, so the g-buffer outputs are declared `vec4`.
  - `rlActiveDrawBuffers(3)` must be called **after** the color attachments are
    attached: rlvk clamps the count to `fbo->colorCount`, which is 0 before.
  - The GL texture-unit setup for the `gPosition/gNormal/gAlbedoSpec` samplers is
    gone; rlvk resolves samplers by descriptor binding (set 1, binding 0/1/2).
  - `rlLoadDrawQuad()` does not exist in rlvk; the fullscreen quad is emitted
    through the immediate-mode batch in clip space (the deferred VS ignores mvp),
    with `rlSetTexture(gPosition)` so the batch does not override sampler slot 0.
  - `RL_READ_FRAMEBUFFER`/`RL_DRAW_FRAMEBUFFER` are not exported; the raw GL enums
    (0x8CA8/0x8CA9) are passed to `rlBindFramebuffer()`.
  - The g-buffer textures are unbound from their sampler slots after the deferred
    pass, otherwise the next frame's geometry pass samples them while they are
    color attachments (Vulkan validation error, see below).

## Library gaps / bugs found (nothing in src/ was modified)

1. **`LoadShader(0, fs)` does not fall back to the default vertex shader.**
   `rcore.c:LoadShader()` passes `vsSpirv = NULL` to `rlLoadShaderCodeSize()`,
   which rejects NULL blobs ("SHADER: Invalid SPIR-V blobs provided") and returns
   0, so the example silently falls back to the default shader. rlvk already has
   `default_vert_spv_data` embedded (used for `RLVK.defaultShaderId`), so the fix
   is to substitute it when `vsSpirv == NULL`. `PORTING.md` documents
   `LoadShader(0, "...frag.spv")` as the expected form.
   Repro: any fragment-only example before the workaround, e.g.
   `LoadShader(0, "resources/shaders/vk/julia_set.frag.spv")`.

2. **Sampling a depth texture attached to an FBO hits an undefined image layout.**
   `shaders_depth_rendering` run with `VK_LOADER_LAYERS_ENABLE='*validation*'`
   reports `VUID-vkCmdDraw-None-09600`: the depth image
   (`rlLoadTextureDepth(w, h, false)`, attached with `RL_ATTACHMENT_DEPTH` /
   `RL_ATTACHMENT_TEXTURE2D`) is still in `VK_IMAGE_LAYOUT_UNDEFINED` when it is
   sampled, although `rlvkEndRenderPass()` has a branch that should transition
   sampleable depth attachments to `SHADER_READ_ONLY_OPTIMAL`. The example renders
   correctly-looking output and logs no error, so this is a latent correctness bug
   in the layout tracking rather than a hard failure. (Message count stops at 10 =
   the validation layer's duplicate-message limit, so it happens every frame.)
   Repro: `VK_LOADER_LAYERS_ENABLE='*validation*' ./shaders_depth_rendering`.

3. **`rlBlitFramebuffer()` only implements the color bit.**
   `if ((bufferMask & RLVK_GL_COLOR_BUFFER_BIT) == 0) return;` - the depth blit
   `shaders_deferred_rendering` uses to copy the g-buffer depth onto the
   swapchain is a silent no-op, so the light-position spheres of the final pass
   are not depth-tested against the deferred scene.

4. **No `rlLoadDrawQuad()`** (used by deferred/postprocessing style examples) and
   no `RL_CULL_FACE_*`, `RL_READ_FRAMEBUFFER`, `RL_DRAW_FRAMEBUFFER`, `RL_FLOAT`,
   `RL_PIXELFORMAT_*`, `RL_SHADER_UNIFORM_*` compatibility defines in `rlvk.h`.
   All were worked around example-side; adding the plain defines to the rlgl shim
   would keep upstream diffs smaller.

5. **Uniform reflection cannot address members of a struct array.**
   `rlGetLocationUniform("lights[0].enabled")` splits at `[`, looks up the base
   name `lights` and returns `offset + index*arrayStride`, i.e. the offset of the
   whole element. Every raylib lighting example uses that naming, so each one has
   to be rewritten to flat arrays. Supporting `base[i].member` in the reflection
   table would let `rlights.h` stay untouched.
