#!/usr/bin/env python3
"""
patch_smali.py — finds the smali call site for
    System.loadLibrary("minecraftpe")
and injects, immediately after it, the same call for
    System.loadLibrary("vinzshader")

Load order matters: our .so's JNI_OnLoad walks libminecraftpe.so's own
relocation table to install the eglSwapBuffers hook, so libminecraftpe.so
MUST already be mapped when JNI_OnLoad runs. Injecting right after the
original loadLibrary call (rather than earlier, e.g. in attachBaseContext)
guarantees that ordering.

Usage: python3 patch_smali.py <decoded_apk_dir>
Exits non-zero (and changes nothing) if the call site isn't found —
better to fail loudly than silently ship an APK where the hook never
installs.
"""
import re
import sys
from pathlib import Path

TARGET_LIB_STRING = "minecraftpe"
HOOK_LIB_NAME = "vinzshader"


def find_and_patch(decoded_dir: Path) -> bool:
    smali_dirs = list(decoded_dir.glob("smali*"))
    if not smali_dirs:
        print(f"ERROR: no smali*/ directories found under {decoded_dir}", file=sys.stderr)
        return False

    const_re = re.compile(r'(\s*)const-string(/jumbo)?\s+(v\d+|p\d+),\s*"' + re.escape(TARGET_LIB_STRING) + r'"')
    invoke_re = re.compile(r'\s*invoke-static\s*\{(v\d+|p\d+)\},\s*Ljava/lang/System;->loadLibrary\(Ljava/lang/String;\)V')

    for smali_root in smali_dirs:
        for smali_file in smali_root.rglob("*.smali"):
            text = smali_file.read_text(encoding="utf-8", errors="surrogateescape")
            lines = text.split("\n")

            for i in range(len(lines)):
                m_const = const_re.match(lines[i])
                if not m_const:
                    continue
                reg = m_const.group(3)

                # the invoke-static usually sits on the very next line, but
                # tolerate a few blank/comment lines in between just in case
                invoke_idx = None
                for j in range(i + 1, min(i + 5, len(lines))):
                    stripped = lines[j].strip()
                    if stripped == "" or stripped.startswith("#") or stripped.startswith(".line"):
                        continue
                    m_invoke = invoke_re.match(lines[j])
                    if m_invoke and m_invoke.group(1) == reg:
                        invoke_idx = j
                    break  # first non-blank line decides it either way

                if invoke_idx is None:
                    continue

                indent = m_const.group(1)
                injected = [
                    f'{indent}const-string {reg}, "{HOOK_LIB_NAME}"',
                    f'{indent}invoke-static {{{reg}}}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V',
                ]
                new_lines = lines[:invoke_idx + 1] + injected + lines[invoke_idx + 1:]
                smali_file.write_text("\n".join(new_lines), encoding="utf-8", errors="surrogateescape")

                print(f"Patched {smali_file.relative_to(decoded_dir)} "
                      f"(line {invoke_idx + 1}), register {reg}")
                return True

    print(f'ERROR: could not find System.loadLibrary("{TARGET_LIB_STRING}") call site. '
          f'The call may be split across more registers/lines than this script handles — '
          f'open the smali for the main Activity/Application class by hand and add:\n'
          f'  const-string v0, "{HOOK_LIB_NAME}"\n'
          f'  invoke-static {{v0}}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V\n'
          f'right after the existing loadLibrary("{TARGET_LIB_STRING}") call.', file=sys.stderr)
    return False


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: patch_smali.py <decoded_apk_dir>", file=sys.stderr)
        sys.exit(1)

    ok = find_and_patch(Path(sys.argv[1]))
    sys.exit(0 if ok else 1)
