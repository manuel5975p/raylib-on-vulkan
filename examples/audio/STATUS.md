# audio — porting status

11/11 examples ported, built and headless-verified: **11 PASS, 0 FAIL**.
(`raygui.h` is a header dependency, not an example.)

Verified per PORTING.md: private compositor (`--socket rayvk-misc`), 8 s timeout,
run from the build-tree category dir. **All runs used `PULSE_SINK=rayvk_null`** so the
audio device opens for real (exercising raudio end to end) without anything being
audible. PASS = "RLVK: Vulkan renderer initialized successfully" present, exit 124
(still running when killed), no ERROR / validation / crash lines, and no non-benign
WARNING — miniaudio's device-configuration dump is emitted at WARNING level by
raudio itself and is ignored.

Every example reached `AUDIO: Device initialized successfully` (miniaudio /
PulseAudio backend), so playback paths, streams and the mixed/processor callbacks
all ran; none of them are stubbed out.

| example | result |
| --- | --- |
| audio_amp_envelope | PASS |
| audio_mixed_processor | PASS |
| audio_module_playing | PASS |
| audio_music_stream | PASS |
| audio_raw_stream | PASS |
| audio_sound_loading | PASS |
| audio_sound_multi | PASS |
| audio_sound_positioning | PASS |
| audio_spectrum_visualizer | PASS |
| audio_stream_callback | PASS |
| audio_stream_effects | PASS |

## Exclusions
None. See `exclude.cmake`.

## Porting deltas

One file, one line: **audio_spectrum_visualizer.c**

    LoadShader(0, TextFormat("resources/shaders/glsl%i/fft.fs", GLSL_VERSION))  ->
    LoadShader("resources/shaders/vk/base.vert.spv", "resources/shaders/vk/fft.frag.spv")

The explicit vertex shader is a **workaround for a library gap**, not a porting
requirement: `LoadShader(NULL, fs)` should reuse the built-in vertex shader (raylib
semantics, and the SPIR-V is already embedded as `default_vert_spv_data`), but
`rlLoadShaderCodeSize()` rejects a NULL `vsSpirv` outright
(src/rlvk.c, "SHADER: Invalid SPIR-V blobs provided"). Once that is fixed, the
`base.vert.spv` argument can be replaced by `0` and `base.vert` deleted.

All other 10 examples are byte-identical to upstream.

## Shaders (`resources/shaders/vk/`)

- `fft.frag` — port of glsl330/fft.fs. `iResolution` is the single set 0, binding 0
  UBO member (name preserved, so `GetShaderLocation(shader, "iResolution")` resolves).
- `base.vert` — verbatim copy of `src/shaders/default.vert` (see workaround above).

**`iChannel0` is declared at set 1, binding _1_, not binding 0.** This is deliberate
and is the one non-obvious part of the port. The example does:

    BeginShaderMode(shader);
        SetShaderValueTexture(shader, iChannel0Location, fftTexture);
        DrawTextureRec(bufferA.texture, ...);
    EndShaderMode();

Binding 0 is the batch texture slot, which `DrawTextureRec()` rebinds to
`bufferA.texture` on every draw — with `iChannel0` at binding 0 it would clobber the
FFT texture and the shader sampled an empty white render target, rendering a blank
white screen. GL raylib does not hit this because `rlSetUniformSampler()` assigns
sampler uniforms to texture units >= 1, leaving unit 0 to the batch; under the Vulkan
ABI the binding is fixed by the shader source, so the shader must declare it. Moving
the declaration to binding 1 needs **no change to the .c file** — the name is
unchanged and `GetShaderLocation()` returns the encoded binding. See the comment in
`fft.frag`.

`fft.frag` uses `fragTexCoord`, not `gl_FragCoord`, so the swapchain Y-origin flip
noted in PORTING.md does not apply.

## Visual verification

`audio_spectrum_visualizer` was additionally captured with a temporary
`TakeScreenshot()` call (since reverted; kwin's virtual backend does not support the
wlr screencopy protocol that `grim` needs). It renders the expected black spectrum
bars rising from the bottom edge with energy concentrated in the low bins, matching
the upstream reference PNG. This capture is what exposed the binding-0 collision
above — the log-only protocol reported a clean PASS while the screen was blank white.
