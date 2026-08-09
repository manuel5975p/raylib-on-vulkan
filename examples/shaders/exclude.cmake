# Per-category exclusions for examples-upstream/shaders

# ---- A half ----
# (shaders_a* .. shaders_l*: none excluded, all 16 examples build and run)
# ---- end A half ----

# ---- B half ----
list(APPEND RAYVK_EXAMPLES_EXCLUDE_EXTRA
    shaders_rlgl_compute        # needs compute shaders + SSBOs (rlLoadShaderProgramCompute/rlLoadShaderBuffer): no compute API in rlvk
)
# ---- end B half ----
