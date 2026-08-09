# core examples - raylib-on-vulkan port status

Verified headless per examples-upstream/PORTING.md (kwin_wayland --virtual, socket
`rayvk-core`, `timeout 8`). PASS = "RLVK: Vulkan renderer initialized successfully"
in the log, no crash, no Vulkan/validation error, exit 124 (idle interactive loop)
or clean 0.

Totals: 49 built, 49 PASS, 0 FAIL, 2 EXCLUDED (globally, in CMakeLists.txt).

| Example | Status | Note |
|---|---|---|
| core_2d_camera | PASS | |
| core_2d_camera_mouse_zoom | PASS | |
| core_2d_camera_platformer | PASS | |
| core_2d_camera_split_screen | PASS | render textures + scissor |
| core_3d_camera_first_person | PASS | |
| core_3d_camera_fps | PASS | |
| core_3d_camera_free | PASS | |
| core_3d_camera_mode | PASS | |
| core_3d_camera_split_screen | PASS | render textures |
| core_3d_picking | PASS | |
| core_automation_events | PASS | interactive (record/play on keypress) |
| core_basic_screen_manager | PASS | no shaders needed |
| core_basic_window | PASS | |
| core_clipboard_text | PASS | |
| core_compute_hash | PASS | CPU-only hashing |
| core_custom_frame_control | PASS | manual SwapScreenBuffer/PollInputEvents |
| core_custom_logging | PASS | stdout is block-buffered; use `stdbuf -oL` when redirecting or the log looks empty after SIGTERM |
| core_delta_time | PASS | |
| core_directory_files | PASS | |
| core_drop_files | PASS | needs a real drop to exercise; init clean |
| core_highdpi_demo | PASS | |
| core_highdpi_testbed | PASS | |
| core_input_actions | PASS | |
| core_input_gamepad | PASS | no gamepad present; no crash |
| core_input_gestures | PASS | needs touch input; no crash |
| core_input_gestures_testbed | PASS | needs touch input; no crash |
| core_input_keys | PASS | |
| core_input_mouse | PASS | |
| core_input_mouse_wheel | PASS | |
| core_input_multitouch | PASS | |
| core_input_virtual_controls | PASS | |
| core_keyboard_testbed | PASS | |
| core_monitor_detector | PASS | |
| core_random_sequence | PASS | |
| core_random_values | PASS | |
| core_render_texture | PASS | |
| core_scissor_test | PASS | |
| core_screen_recording | PASS | msf_gif; recording starts on keypress |
| core_smooth_pixelperfect | PASS | |
| core_storage_values | PASS | |
| core_text_file_loading | PASS | |
| core_undo_redo | PASS | |
| core_viewport_scaling | PASS | |
| core_vr_simulator | PASS | PORTED: distortion.fs -> resources/shaders/vk/distortion.{vert,frag}; rlvk logs "VR: Stereo rendering is not supported by the Vulkan backend, rendering left eye only" (backend limitation, not a port defect) |
| core_window_flags | PASS | |
| core_window_letterbox | PASS | |
| core_window_should_close | PASS | |
| core_window_web | PASS | |
| core_world_screen | PASS | |
| core_basic_window_web | EXCLUDED | web/emscripten-only (CMakeLists.txt) |
| core_input_gestures_web | EXCLUDED | web/emscripten-only (CMakeLists.txt) |

## Porting deltas

- `core_vr_simulator.c`: loads SPIR-V instead of GLSL. Two lines changed
  (`LoadShader(...)`). It needs an explicit vertex shader because
  `LoadShader(0, fs)` is rejected by the backend (see below), so
  `resources/shaders/vk/distortion.vert` is a verbatim copy of
  `src/shaders/default.vert`.

## Library issue found (not fixed here - src/ is out of scope)

`LoadShader(NULL, fsFileName)` fails. `src/rcore.c:LoadShader()` (~line 1082) passes
`vsSpirv == NULL, vsSize == 0` straight to `rlLoadShaderCodeSize()`, which rejects
NULL blobs at `src/rlvk.c:2527` ("SHADER: Invalid SPIR-V blobs provided"). Upstream
raylib substitutes the default vertex shader in that case, and PORTING.md documents
the default-VS varying layout as if the pairing works.
Repro: `Shader s = LoadShader(0, "resources/shaders/vk/distortion.frag.spv");` ->
`s.id == 0` + "SHADER: Failed to load shader code [internal, ...]".
Fix would be in `rcore.c LoadShader()`/`rlLoadShaderCodeSize()`: fall back to the
default vertex module when `vsSpirv == NULL`. Every upstream example using the
`LoadShader(0, fs)` idiom (most of the `shaders/` category) is affected.
