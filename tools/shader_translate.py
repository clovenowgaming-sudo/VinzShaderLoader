#!/usr/bin/env python3
"""
shader_translate.py — VinzShaderLoader

Translates a modern post-process GLSL fragment shader (Iris/OptiFine-style
"final.fsh" / "composite*.fsh", written for desktop GLSL 1.30+/3.30 core)
down to GLSL ES 1.00, so it can be compiled and run inside an old GLES2
engine (Minecraft PE alpha, this project's actual target) via the native
post-process hook in native/src/gl_hook.c.

WHAT THIS DOES NOT DO (be honest with yourself before relying on it):
  - It does not implement a real multi-pass deferred renderer. Old MCPE
    only has ONE opaque forward-rendered frame available to us (the
    default framebuffer at eglSwapBuffers time). So only a shader that
    operates purely on scene color (+ optionally scene depth) as a single
    post-process pass can be meaningfully ported. Shadow maps, gbuffer
    normal/albedo buffers, multiple composite stages chained together —
    none of that exists on this engine, so references to them are
    stubbed out (replaced with a harmless constant) rather than faked
    into something they're not.
  - It is a pragmatic regex/line-based transpiler, not a real GLSL
    parser. It handles the patterns that actually show up in real
    shaderpacks' final composite pass. Complex control flow or macro
    trickery may need a manual touch-up after translation — the report
    printed at the end tells you exactly what to check.

Usage:
    python3 shader_translate.py <input.fsh> <output.fsh> [--stage fragment|vertex]
"""

import argparse
import re
import sys
from dataclasses import dataclass, field


# Uniform names from the Iris/OptiFine ecosystem that we CAN actually
# back with something real on the old engine, mapped to the uniform
# names our native hook (gl_hook.c) binds every frame.
SUPPORTED_UNIFORM_MAP = {
    "colortex0": "uSceneColor",     # the frame we captured before swap
    "gcolor": "uSceneColor",        # older OptiFine alias for colortex0
    "depthtex0": "uSceneDepth",     # depth buffer, IF the engine exposes one (see README)
    "gdepth": "uSceneDepth",
    "texelSize": "uTexelSize",      # 1.0 / resolution, we compute this ourselves
    "frameTimeCounter": "uTime",
    "viewWidth": "uResolution.x",
    "viewHeight": "uResolution.y",
}

# Anything referencing these doesn't exist on the old forward renderer.
# We don't try to fake shadow maps or gbuffer data — we stub the sampler
# reads to a neutral value so the shader still COMPILES and RUNS, and we
# tell the user exactly what got stubbed so they know the visual result
# is an approximation, not a lie.
UNSUPPORTED_SAMPLERS = [
    "shadowtex0", "shadowtex1", "shadowcolor0", "shadowcolor1",
    "colortex1", "colortex2", "colortex3", "colortex4", "colortex5",
    "gnormal", "gaux1", "gaux2", "gaux3", "gaux4",
    "depthtex1", "depthtex2",
]


@dataclass
class TranslationReport:
    version_from: str = ""
    version_to: str = "100"
    renamed_uniforms: dict = field(default_factory=dict)
    stubbed_samplers: set = field(default_factory=set)
    stubbed_lines: list = field(default_factory=list)
    warnings: list = field(default_factory=list)

    def print_report(self, out=sys.stderr):
        print("=== VinzShaderLoader translation report ===", file=out)
        print(f"  GLSL version: {self.version_from or '(implicit 110)'} -> {self.version_to}", file=out)
        if self.renamed_uniforms:
            print("  Uniforms mapped to native post-process pipeline:", file=out)
            for src, dst in sorted(self.renamed_uniforms.items()):
                print(f"    {src} -> {dst}", file=out)
        if self.stubbed_samplers:
            print("  Stubbed (not available on old renderer, replaced with neutral value):", file=out)
            for s in sorted(self.stubbed_samplers):
                print(f"    {s}", file=out)
        if self.stubbed_lines:
            print("  Lines touched by stubbing (check these manually):", file=out)
            for ln, text in self.stubbed_lines:
                print(f"    L{ln}: {text.strip()}", file=out)
        for w in self.warnings:
            print(f"  WARNING: {w}", file=out)
        print("============================================", file=out)


def strip_layout_qualifiers(src: str) -> str:
    # layout(location = 0) out vec4 fragColor;  ->  out vec4 fragColor;  (then handled below)
    return re.sub(r"layout\s*\([^)]*\)\s*", "", src)


def translate_version(src: str, report: TranslationReport):
    m = re.search(r"#version\s+(\d+)\s*(core|es|compatibility)?", src)
    if m:
        report.version_from = m.group(0)
        src = src[:m.start()] + "#version 100" + src[m.end():]
    else:
        src = "#version 100\n" + src
    return src


def translate_io_qualifiers(src: str, stage: str, report: TranslationReport) -> str:
    """
    GLSL 150+/330: `in` for stage inputs, `out` for stage outputs.
    GLSL ES 100:   `attribute`/`varying` (vertex) or `varying` (fragment),
                   and gl_FragColor instead of a user-declared `out vec4`.
    """
    out_var_names = []

    def repl_out_decl(m):
        qualifiers, type_, name = m.group(1), m.group(2), m.group(3)
        out_var_names.append(name)
        # fragment shader outputs become gl_FragColor; drop the declaration
        if stage == "fragment":
            return ""
        return f"varying {type_} {name};"

    # out <type> <name>; at global scope
    src = re.sub(r"\n\s*out\s+(\w+\s+)?(\w+)\s+(\w+)\s*;", lambda m: repl_out_decl(
        type("m", (), {"group": lambda self, i: [None, m.group(1), m.group(2), m.group(3)][i]})()
    ), "\n" + src)

    # redo more simply and robustly: separate passes for 'in' and 'out'
    def in_decl_repl(m):
        type_, name = m.group(1), m.group(2)
        kw = "attribute" if stage == "vertex" else "varying"
        return f"{kw} {type_} {name};"

    src = re.sub(r"\bin\s+(\w+)\s+(\w+)\s*;", in_decl_repl, src)

    def out_decl_repl(m):
        type_, name = m.group(1), m.group(2)
        out_var_names.append(name)
        if stage == "fragment":
            return "// (removed) out %s %s; -> written via gl_FragColor" % (type_, name)
        return f"varying {type_} {name};"

    src = re.sub(r"\bout\s+(\w+)\s+(\w+)\s*;", out_decl_repl, src)

    if stage == "fragment":
        for name in out_var_names:
            src = re.sub(rf"\b{name}\s*=", "gl_FragColor =", src)

    return src


def translate_texture_calls(src: str) -> str:
    # texture(sampler, uv) / texture(sampler, uv, bias) -> texture2D(sampler, uv)
    src = re.sub(r"\btexture\s*\(\s*([^,]+?)\s*,\s*([^,)]+?)\s*(?:,\s*[^)]+)?\)", r"texture2D(\1, \2)", src)
    # textureLod(sampler, uv, lod) -> texture2D(sampler, uv)  (ES2 has no fragment LOD control)
    src = re.sub(r"\btextureLod\s*\(\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*[^)]+\)", r"texture2D(\1, \2)", src)
    # texelFetch(sampler, ivec2, lod) has no direct ES2 equivalent on a plain sampler2D;
    # approximate with a texture2D using normalized coords is not generally valid without
    # texel size info, so leave it for the report to flag via caller.
    return src


def translate_precision(src: str, stage: str) -> str:
    if "precision " not in src:
        default_prec = "precision mediump float;\n" if stage == "fragment" else ""
        # insert right after #version line
        lines = src.split("\n")
        for i, l in enumerate(lines):
            if l.strip().startswith("#version"):
                lines.insert(i + 1, default_prec.strip())
                break
        src = "\n".join(lines)
    return src


def apply_uniform_mapping(src: str, report: TranslationReport) -> str:
    for src_name, dst_name in SUPPORTED_UNIFORM_MAP.items():
        pattern = r"\b" + re.escape(src_name) + r"\b"
        if re.search(pattern, src):
            src = re.sub(pattern, dst_name, src)
            report.renamed_uniforms[src_name] = dst_name
    # drop now-redundant uniform declarations for things we renamed to
    # built-ins we provide ourselves as plain uniforms with new names —
    # keep the declaration line but rename the type-matching identifier;
    # simplest robust approach: just strip any uniform decl whose name
    # was one of our source keys (we've already replaced usages above,
    # so the OLD decl line, now holding the OLD name only in comments,
    # can be dropped safely since it was never referenced again).
    for src_name in SUPPORTED_UNIFORM_MAP:
        src = re.sub(rf"uniform\s+\w+\s+{re.escape(src_name)}\s*;\n?", "", src)
    return src


def stub_unsupported_samplers(src: str, report: TranslationReport) -> str:
    lines = src.split("\n")
    for name in UNSUPPORTED_SAMPLERS:
        decl_pattern = re.compile(rf"uniform\s+sampler2D\s+{re.escape(name)}\s*;")
        use_pattern = re.compile(rf"\btexture2D\s*\(\s*{re.escape(name)}\s*,[^)]*\)")
        found = False
        for i, line in enumerate(lines):
            if decl_pattern.search(line):
                lines[i] = f"// (stubbed, not available on old renderer) {line.strip()}"
                found = True
            if use_pattern.search(line):
                new_line = use_pattern.sub("vec4(0.0)", line)
                if new_line != line:
                    report.stubbed_lines.append((i + 1, line))
                    lines[i] = new_line
                    found = True
        if found:
            report.stubbed_samplers.add(name)
    return "\n".join(lines)


def translate(src: str, stage: str) -> (str, TranslationReport):
    report = TranslationReport()
    src = strip_layout_qualifiers(src)
    src = translate_version(src, report)
    src = translate_io_qualifiers(src, stage, report)
    src = translate_texture_calls(src)
    src = apply_uniform_mapping(src, report)
    src = stub_unsupported_samplers(src, report)
    src = translate_precision(src, stage)
    # #version must be the very first thing in the file, no leading
    # blank lines or whitespace — several of the regex passes above can
    # leave a stray leading newline, so strip it before returning.
    src = src.lstrip("\n")

    if "texelFetch" in src:
        report.warnings.append(
            "texelFetch() found — no direct GLES 1.00 equivalent on a plain sampler2D; "
            "left as-is, this line will fail to compile. Replace with texture2D() and "
            "normalized UVs by hand."
        )
    if re.search(r"\bswitch\b", src):
        report.warnings.append("switch statement found — not supported in GLSL ES 1.00, rewrite as if/else.")

    return src, report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--stage", choices=["fragment", "vertex"], default="fragment")
    args = ap.parse_args()

    with open(args.input, "r") as f:
        src = f.read()

    out_src, report = translate(src, args.stage)

    with open(args.output, "w") as f:
        f.write(out_src)

    report.print_report()
    print(f"\nWrote translated {args.stage} shader -> {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
