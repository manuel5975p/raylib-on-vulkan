# textures examples — raylib-on-vulkan port status

Verified headless (kwin_wayland --virtual, socket `rayvk-textures`), 8 s timeout each.
PASS = "RLVK: Vulkan renderer initialized successfully" in log, no Vulkan ERROR, no crash,
exit 124 (still running when killed).

Totals: 32 examples, 32 PASS, 0 FAIL, 0 excluded. No runtime GLSL shaders in this category.

| Example | Status | Porting delta |
|---|---|---|
| textures_background_scrolling | PASS | none |
| textures_blend_modes | PASS | none |
| textures_bunnymark | PASS | none |
| textures_cellular_automata | PASS | none |
| textures_clipboard_image | PASS | none |
| textures_fog_of_war | PASS | none |
| textures_framebuffer_rendering | PASS | none (logs benign Wayland SetMousePosition notice) |
| textures_gif_player | PASS | none |
| textures_image_channel | PASS | none |
| textures_image_drawing | PASS | none |
| textures_image_generation | PASS | none |
| textures_image_kernel | PASS | none |
| textures_image_loading | PASS | none |
| textures_image_processing | PASS | none |
| textures_image_rotate | PASS | none |
| textures_image_text | PASS | none |
| textures_logo_raylib | PASS | none |
| textures_magnifying_glass | PASS | added local `RL_ZERO`/`RL_ONE`/`RL_FUNC_ADD` defines (see note) |
| textures_mouse_painting | PASS | none |
| textures_npatch_drawing | PASS | none |
| textures_particles_blending | PASS | none |
| textures_polygon_drawing | PASS | none (rlgl.h shim; rlBegin/rlEnd/rlSetTexture all present) |
| textures_raw_data | PASS | none |
| textures_screen_buffer | PASS | none |
| textures_sprite_animation | PASS | none |
| textures_sprite_button | PASS | none (miniaudio device chatter in log is benign) |
| textures_sprite_explosion | PASS | none (miniaudio device chatter in log is benign) |
| textures_sprite_stacking | PASS | none |
| textures_srcrec_dstrec | PASS | none |
| textures_textured_curve | PASS | none (rlgl.h shim) |
| textures_tiled_drawing | PASS | none |
| textures_to_image | PASS | none |

## Library gap (not a bug, but worth filing)

`src/rlvk.h` declares `rlSetBlendFactors()` / `rlSetBlendFactorsSeparate()` as taking
"GL-style enums" but does not export the corresponding `RL_ZERO`, `RL_ONE`,
`RL_FUNC_ADD`, … constants that upstream rlgl.h provides. The values are defined
privately in `src/rlvk.c` as `RLVK_GL_*`. `textures_magnifying_glass` was adapted by
defining the three constants it needs locally; exporting the full `RL_*` blend enum set
from `rlvk.h` (or the rlgl shim) would remove the need for that in future examples.
