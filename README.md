# VinzShaderLoader

A shader-translation + injection pipeline for **Minecraft PE alpha
0.15.10** (`PE-a0_15_10.apk` — armeabi-v7a, monolithic
`libminecraftpe.so`, GLES2 programmable pipeline).

## What this actually does

1. **`tools/shader_translate.py`** — takes a modern, *real, open-source*
   post-process GLSL shader (Iris/OptiFine-style `final.fsh`, the format
   BSL / SEUS / Complementary and friends actually ship as plain text —
   this is the real ecosystem, not Mojang's closed Bedrock RenderDragon
   bytecode) and transpiles it down to GLSL ES 1.00: version directive,
   `in`/`out` → `attribute`/`varying`, `texture()` → `texture2D()`,
   uniform renames (`colortex0` → `uSceneColor`, etc.), and stubs out
   anything the old renderer genuinely can't supply (shadow maps, extra
   gbuffer textures) rather than faking it.
2. **`native/`** — `libvinzshader.so`. A from-scratch ARM32 ELF
   PLT/GOT hooker (`elf_hook.c`, same technique as the open xHook
   project) hooks `eglSwapBuffers` as called *from* `libminecraftpe.so`.
   Every frame, right before the real swap: capture the just-rendered
   frame into a texture (`glCopyTexImage2D`), run your translated
   fragment shader as a fullscreen post-process pass, draw the result
   back to the default framebuffer, *then* call the real
   `eglSwapBuffers`.
3. **`patch/`** — repacks the target APK: injects `libvinzshader.so`
   and one smali edit that loads it (`System.loadLibrary("vinzshader")`)
   immediately after the game's own `loadLibrary("minecraftpe")` call,
   so our hook installs only once the target library is already mapped.

## Be clear-eyed about the limits

- **No multi-pass deferred pipeline.** Real shaderpacks chain several
  composite passes (shadow pass → gbuffer → composite1..N → final) over
  separate render targets. Old MCPE only ever gives us ONE flat,
  already-shaded frame at swap time. Only the *final* pass — pure
  screen-space post-processing (bloom, tonemap, color grading, vignette,
  chromatic aberration, water-style distortion) — can be meaningfully
  ported. That's what the translator targets, and it's genuinely what
  most of the "visual pop" in a shaderpack comes from anyway.
- **No real depth buffer.** The default framebuffer's depth isn't
  readable without the engine itself exposing an FBO with a depth
  texture attachment, which this build doesn't have. `uSceneDepth` is
  bound to a dummy 1×1 white (far-plane) texture — depth-dependent
  effects (fog falloff, SSAO) will not look right. Flagged clearly in
  the translator's report and in `gl_hook.c`.
- **No real shadow maps.** Same reason. Stubbed to `vec4(0.0)`.
- **This is a transpiler, not a parser.** It's regex/pattern based and
  handles what real final-pass shaders actually contain. Exotic control
  flow (`switch`, `texelFetch` on arbitrary textures) will be flagged in
  the report rather than silently mistranslated — fix those lines by
  hand before shipping a pack.

## Build it

```bash
# 1. Native hook library — either locally with the NDK, or push to
#    GitHub and let .github/workflows/build-native.yml build it for you
#    (same pattern as your other Termux -> GH Actions projects).
cmake -S native -B build -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-19
cmake --build build

# 2. Translate a shaderpack's final pass (repeat per pack you want to support)
python3 tools/shader_translate.py shaderpacks/sample_source/final.fsh \
    shaderpacks/sample_translated/final.fsh --stage fragment

# 3. Patch the APK
chmod +x patch/patch_apk.sh
./patch/patch_apk.sh PE-a0_15_10.apk build/libvinzshader.so PE-a0_15_10-shaders.apk
```

## Install shaderpacks on-device

Shaderpacks are NOT baked into the APK — they live on storage so you can
swap them without re-patching:

```
/sdcard/VinzShader/shaderpacks/<pack_name>/final.fsh   (translated output)
/sdcard/VinzShader/shaderpacks/<pack_name>/pack.json    (optional metadata)
/sdcard/VinzShader/active_pack.txt                      (one line: the pack_name to use)
```

Push the included demo with:

```bash
adb push shaderpacks/sample_translated /sdcard/VinzShader/shaderpacks/sample_translated
echo sample_translated > /sdcard/VinzShader/active_pack.txt
adb push /sdcard/VinzShader/active_pack.txt /sdcard/VinzShader/active_pack.txt
```

Install the patched APK, launch, check `adb logcat -s VinzShader` — you
should see `elf_hook: hooked 'eglSwapBuffers'` and
`shader_loader: loaded shaderpack 'sample_translated'`.

## Getting real shaderpacks to feed the translator

Grab any open-source Iris/OptiFine pack (BSL, Complementary, SEUS Renewed,
etc.) and point the translator at its `final.fsh` (sometimes named
`composite_final.fsh` or just the highest-numbered `compositeN.fsh` if
there's no explicit final pass). Expect to hand-fix a few lines per
pack — the report tells you exactly which.

## Next steps worth building (not done here)

- A second post-process pass (ping-pong between two textures) so two
  cheap effects (e.g. bloom + tonemap) can run as separate shaders
  instead of being hand-merged into one `final.fsh`.
- Hook `glCompileShader`/`glShaderSource` directly to patch the game's
  *own* vertex/fragment shaders (e.g. inject waving foliage into the
  terrain vertex shader) instead of only post-processing — bigger
  payoff, bigger risk of breaking rendering if done wrong.
- An on-device toggle (overlay button) to swap `active_pack.txt` and
  force a pipeline reload without restarting the game.
