#!/data/data/com.termux/files/usr/bin/bash
# patch_apk.sh — repacks the target APK with libvinzshader.so + the
# smali hook that loads it right after the game loads its own native lib.
#
# Usage:
#   ./patch_apk.sh <input.apk> <libvinzshader.so> <output.apk>
#
# Requires (install in Termux): apktool, zipalign, apksigner, python3
#   pkg install apktool  # (or: pkg install aapt; see README for manual apktool.jar setup)
# zipalign/apksigner come from `pkg install android-tools` or the
# command-line SDK build-tools you already use for your other APK projects.

set -euo pipefail

IN_APK="${1:?usage: patch_apk.sh <input.apk> <libvinzshader.so> <output.apk>}"
HOOK_SO="${2:?missing libvinzshader.so path}"
OUT_APK="${3:?missing output apk path}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "[1/6] Decoding $IN_APK ..."
apktool d -f -o "$WORK_DIR/decoded" "$IN_APK"

echo "[2/6] Patching smali load order ..."
python3 "$SCRIPT_DIR/patch_smali.py" "$WORK_DIR/decoded"

echo "[3/6] Injecting libvinzshader.so (armeabi-v7a) ..."
mkdir -p "$WORK_DIR/decoded/lib/armeabi-v7a"
cp "$HOOK_SO" "$WORK_DIR/decoded/lib/armeabi-v7a/libvinzshader.so"

echo "[4/6] Rebuilding APK ..."
apktool b "$WORK_DIR/decoded" -o "$WORK_DIR/unsigned.apk"

echo "[5/6] Zipaligning ..."
zipalign -p 4 "$WORK_DIR/unsigned.apk" "$WORK_DIR/aligned.apk"

echo "[6/6] Signing ..."
KEYSTORE="$SCRIPT_DIR/.vinzshader-debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    echo "  (no debug keystore found, generating one — password: vinzshader)"
    keytool -genkeypair -v -keystore "$KEYSTORE" -storepass vinzshader \
        -alias vinzshader -keypass vinzshader -keyalg RSA -keysize 2048 \
        -validity 36500 -dname "CN=VinzShaderLoader, OU=vinz, O=vinz, L=, S=, C=ID"
fi
apksigner sign --ks "$KEYSTORE" --ks-pass pass:vinzshader --ks-key-alias vinzshader \
    --out "$OUT_APK" "$WORK_DIR/aligned.apk"

echo
echo "Done -> $OUT_APK"
echo "Don't forget: adb push (or just move) your shaderpacks/ to"
echo "  /sdcard/VinzShader/shaderpacks/<pack_name>/final.fsh"
echo "and pick the active one in /sdcard/VinzShader/active_pack.txt"
