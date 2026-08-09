# examples-upstream/models — raylib-on-vulkan port status

30 examples, 30 build, 30 PASS the headless protocol, 0 excluded.

Verification: private compositor (`kwin_wayland --virtual --socket rayvk-models`),
each binary run from `<build>/examples-upstream/models` (CWD needed for `resources/`).
PASS = "RLVK: Vulkan renderer initialized successfully" in the log, no Vulkan ERROR,
exit code 124 (still alive when killed). On top of the protocol every example was
also rendered to a PNG at frame 40 (via a patched scratch copy, not committed) and
compared against the upstream reference screenshot.

## Results

| Example | Build | Run | Visual | Notes |
|---|---|---|---|---|
| models_animation_blend_custom | ok | PASS | ok | shader path ported (skinning.vert/frag) |
| models_animation_blending | ok | PASS | ok | CPU skinning (SUPPORT_GPU_SKINNING off) |
| models_animation_gpu_skinning | ok | PASS | ok | GPU path compiled out by SUPPORT_GPU_SKINNING=0 |
| models_animation_timing | ok | PASS | ok | |
| models_basic_voxel | ok | PASS | ok | |
| models_billboard_rendering | ok | PASS | ok | |
| models_bone_socket | ok | PASS | ok | needs the glTF `materials[1]` fix, see note 6 |
| models_box_collisions | ok | PASS | ok | |
| models_cubicmap_rendering | ok | PASS | ok | |
| models_decals | ok | PASS | ok | |
| models_directional_billboard | ok | PASS | ok | |
| models_first_person_maze | ok | PASS | ok | |
| models_geometric_shapes | ok | PASS | ok | |
| models_heightmap_rendering | ok | PASS | ok | |
| models_loading | ok | PASS | ok | |
| models_loading_gltf | ok | PASS | ok | |
| models_loading_iqm | ok | PASS | **model invisible** | library bug: IQM bone weights (see below) |
| models_loading_m3d | ok | PASS | ok | |
| models_loading_vox | ok | PASS | ok | voxel_lighting shader + rlights ported |
| models_mesh_generation | ok | PASS | ok | |
| models_mesh_picking | ok | PASS | ok | |
| models_orthographic_projection | ok | PASS | ok | |
| models_point_rendering | ok | PASS | ok | rlvk supports POINT_LIST topology |
| models_rlgl_solar_system | ok | PASS | ok | all rl* calls exist in rlvk.h |
| models_rotating_cube | ok | PASS | ok | |
| models_skybox_rendering | ok | PASS | ok | skybox/cubemap shaders ported |
| models_tesseract_view | ok | PASS | ok | |
| models_textured_cube | ok | PASS | ok | |
| models_waving_cubes | ok | PASS | ok | |
| models_yaw_pitch_roll | ok | PASS | ok | |

## Porting deltas (example side)

Shaders ported to `resources/shaders/vk/` (compiled to `.spv` by the build):
`skybox.vert/frag`, `cubemap.vert/frag`, `skinning.vert/frag`,
`voxel_lighting.vert/frag`. The 5 examples that load shaders now load the
`.spv` paths directly.

* **models_point_rendering** — `rlDisablePointMode()` does not exist in rlvk;
  `rlDisableWireMode()` clears both wire and point mode, so it is used instead.
  The two `rlEnableBackfaceCulling()` restores are commented out (see bug 3).
* **models_skybox_rendering** —
  * `rlLoadDrawCube()` does not exist; the internal batch (`DrawCubeV()` +
    `rlDrawRenderBatchActive()`) draws the cube in `GenTextureCubemap()`
    (this is the "ALTERNATIVE" upstream already had commented out).
  * The `SetShaderValue(..., "environmentMap"/"equirectangularMap", texture unit)`
    calls are removed: rlvk binds samplers at fixed `set 1, binding N` slots, so
    there is no texture-unit uniform. Worse, `rlGetLocationUniform()` returns a
    *sampler* location for those names and `rlSetUniform()` then reinterprets the
    int value as a **texture id**, silently rebinding a slot (see bug 4).
  * The cubemap is stored in `maps[MATERIAL_MAP_DIFFUSE]` instead of
    `maps[MATERIAL_MAP_CUBEMAP]` because rlvk exposes only 4 sampler slots while
    `MATERIAL_MAP_CUBEMAP == 7` (see bug 2). `skybox.frag` declares
    `samplerCube environmentMap` at `set 1, binding 0`.
  * `rlEnableBackfaceCulling()` restores commented out (see bug 3).
* **rlights.h** (models copy) — the shader-side `Light lights[]` struct array is
  flattened into `lightsEnabled/lightsType/lightsPosition/lightsTarget/lightsColor`
  arrays, because rlvk's SPIR-V reflection only resolves *top-level* UBO members
  (`GetShaderLocation("lights[0].enabled")` cannot work: the name is split at `[`
  and every member of the struct would map to the same offset).

## Library issues found (NOT fixed here — src/ is out of scope)

1. **IQM bone weights are read as floats but stored as UBYTE** — `src/rmodels.c`
   `LoadIQM()`, `case IQM_BLENDWEIGHTS` (~line 3931) does
   `memcpy(blendw, fileData + va[i].offset, floatCount*sizeof(float))`.
   `resources/models/iqm/guy.iqm` declares that vertex array with
   `format = IQM_UBYTE, size = 4` (bytes `255,0,0,0,...`), so the floats come out
   as denormal garbage. Repro: load `guy.iqm`, sum `mesh.boneWeights` → `0.00`
   (glTF/M3D models sum to `vertexCount`). Effect: `UpdateModelAnimation()`
   collapses every vertex to the origin and **models_loading_iqm renders nothing**;
   skipping `UpdateModelAnimation()` renders the model correctly.
   `IQM_BLENDINDEXES` is handled correctly (read as unsigned char).
   Fix: honour `va[i].format` (UBYTE → `/255.0f`, FLOAT → copy).

2. **Material map slots >= 4 can never be bound, and silently corrupt slot 0** —
   `RLVK_MAX_TEXTURE_SLOTS == 4`, but `BindMaterialTextures()` (`src/rmodels.c`)
   calls `rlActiveTextureSlot(i)` for `i` up to `MAX_MATERIAL_MAPS`.
   `rlActiveTextureSlot()` silently returns for `slot >= 4`, so the following
   `rlEnableTextureCubemap()` writes into whatever slot was active last — for a
   model with a diffuse map that is **slot 0**. So `MATERIAL_MAP_CUBEMAP` (7),
   `IRRADIANCE` (8), `PREFILTER` (9), `BRDF` (10) are unusable and clobber the
   diffuse binding. Repro: the original models_skybox_rendering assignment to
   `maps[MATERIAL_MAP_CUBEMAP]` + `samplerCube` at binding 3 →
   `VUID-vkCmdDrawIndexed-viewType-07752` (2D view bound to a Cube sampler) and a
   black skybox.

3. **`rlEnableBackfaceCulling()` culls all 2D batch geometry** — rlvk starts with
   `cullEnable = false` (rlgl starts with culling *enabled*), and its swapchain
   pipelines use `VK_FRONT_FACE_CLOCKWISE` to compensate for the Y-flipping clip
   correction. That is right for 3D, but raylib's 2D ortho projection flips Y a
   second time, so 2D quads become back-facing: after any
   `rlEnableBackfaceCulling()` every `DrawText`/`DrawRectangle`/`DrawFPS` in the
   frame disappears (3D meshes are unaffected). Minimal repro:
   ```c
   BeginMode3D(cam); rlDisableBackfaceCulling(); rlEnableBackfaceCulling(); EndMode3D();
   DrawText("HELLO", 10, 10, 30, RED);   // never appears
   ```
   `rlSetCullFace(0)` (cull FRONT) brings the text back, confirming the winding
   mismatch. Affects models_point_rendering and models_skybox_rendering upstream.

4. **`rlSetUniform()` reinterprets a sampler location's int value as a texture id** —
   `rlGetLocationUniform()` returns `RLVK_LOC_SAMPLER_FLAG | binding` for sampler
   names, and `rlSetUniform()` forwards to `rlSetUniformSampler(loc, *(unsigned*)value)`.
   In GL/raylib the int passed to `SetShaderValue(..., SHADER_UNIFORM_INT)` for a
   sampler is a *texture unit*, not a texture id, so the idiomatic
   `SetShaderValue(shader, GetShaderLocation(shader,"tex"), (int[1]){2}, SHADER_UNIFORM_INT)`
   binds texture **id 2** into slot 2 and poisons every later draw.
   Suggestion: treat a sampler location + `SHADER_UNIFORM_INT` as a no-op (the
   binding is fixed by the ABI) instead of a texture bind.

5. **POINT_LIST pipelines are created from a VS that never writes `PointSize`** —
   with validation layers enabled models_point_rendering reports
   `VUID-VkGraphicsPipelineCreateInfo-topology-08773`
   ("PointSize is not written to, but Pipeline topology is
   VK_PRIMITIVE_TOPOLOGY_POINT_LIST"). It renders fine on NVIDIA, but the default
   vertex shader (`src/shaders/default.vert`) should write `gl_PointSize = 1.0`
   (or `maintenance5` should be enabled).

6. *(fixed during this session by another change in `src/`)* glTF material
   indexing: `LoadGLTF()` now puts the default material at `materials[0]` and the
   glTF materials at `1..N`, which is what `models_bone_socket` /
   `models_animation_gpu_skinning` rely on (`materials[1]`). Before that fix
   models_bone_socket rendered an untextured white character.
