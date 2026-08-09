# shapes examples - raylib-on-vulkan port status

Verified headless per examples-upstream/PORTING.md (kwin_wayland --virtual, socket
`rayvk-core`, `timeout 8`). PASS = "RLVK: Vulkan renderer initialized successfully"
in the log, no crash, no Vulkan/validation error, exit 124 (idle interactive loop)
or clean 0.

Totals: 43 built, 43 PASS, 0 FAIL, 0 EXCLUDED. No source changes were needed in any
shapes example; no shaders are used by this category. The logs are completely free
of WARNING lines.

| Example | Status | Note |
|---|---|---|
| shapes_ball_physics | PASS | |
| shapes_basic_shapes | PASS | |
| shapes_bouncing_ball | PASS | |
| shapes_bullet_hell | PASS | |
| shapes_circle_sector_drawing | PASS | |
| shapes_clock_of_clocks | PASS | |
| shapes_collision_area | PASS | |
| shapes_colors_palette | PASS | |
| shapes_dashed_line | PASS | |
| shapes_digital_clock | PASS | |
| shapes_double_pendulum | PASS | |
| shapes_easings_ball | PASS | |
| shapes_easings_box | PASS | |
| shapes_easings_rectangles | PASS | |
| shapes_easings_testbed | PASS | |
| shapes_ellipse_collision | PASS | |
| shapes_following_eyes | PASS | |
| shapes_hilbert_curve | PASS | |
| shapes_kaleidoscope | PASS | |
| shapes_lines_bezier | PASS | |
| shapes_lines_drawing | PASS | |
| shapes_logo_raylib | PASS | |
| shapes_logo_raylib_anim | PASS | |
| shapes_math_angle_rotation | PASS | |
| shapes_math_sine_cosine | PASS | |
| shapes_mouse_trail | PASS | |
| shapes_outlines_thickness | PASS | |
| shapes_penrose_tile | PASS | |
| shapes_pie_chart | PASS | |
| shapes_polygon_lines | PASS | |
| shapes_rectangle_advanced | PASS | |
| shapes_rectangle_scaling | PASS | |
| shapes_recursive_tree | PASS | |
| shapes_ring_drawing | PASS | |
| shapes_rlgl_color_wheel | PASS | rlgl.h compat shim, rlBegin/rlVertex path |
| shapes_rlgl_triangle | PASS | rlgl.h compat shim |
| shapes_rounded_rectangle_drawing | PASS | |
| shapes_simple_particles | PASS | |
| shapes_splines_drawing | PASS | |
| shapes_starfield_effect | PASS | |
| shapes_top_down_lights | PASS | BLEND_ADDITIVE / render texture path |
| shapes_triangle_strip | PASS | |
| shapes_vector_angle | PASS | |
