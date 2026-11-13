#ifndef VSL_ELF_HOOK_H
#define VSL_ELF_HOOK_H

/*
 * Minimal from-scratch ELF32 PLT/GOT hooker for Android ARM32.
 *
 * Why this exists instead of pulling in bytehook/xhook/whale: the whole
 * point of this project is a single self-contained .so with no runtime
 * dependency downloads needed inside the repacked APK, and this is a
 * well-understood, long-published technique (same idea as the open
 * xHook project) — find the target library's relocation table, locate
 * the GOT slot resolving to the symbol we want, and overwrite it with
 * our own trampoline, after mprotect'ing the page writable.
 *
 * It only works for calls that go through the PLT (i.e. calls to
 * functions NOT defined in the hooked library itself — eglSwapBuffers,
 * glCompileShader, etc. all qualify, since they live in libEGL.so /
 * libGLESv2.so and libminecraftpe.so just calls out to them).
 */

#include <stddef.h>

typedef struct {
    const char *lib_name_substr; // substring to match in /proc/self/maps, e.g. "libminecraftpe.so"
    const char *symbol_name;     // exact symbol name to hook, e.g. "eglSwapBuffers"
    void *replacement;           // our hook function
    void **original_out;         // receives the original function pointer, for calling through
} vsl_hook_request;

/*
 * Hooks one symbol as called FROM the library matching lib_name_substr.
 * Returns 0 on success, negative on failure (symbol not found, lib not
 * loaded yet, relocation table missing, etc.) — failures are logged via
 * log.h, never fatal, so a missing hook degrades to "no post-process"
 * rather than crashing the game.
 */
int vsl_hook_symbol(const vsl_hook_request *req);

#endif // VSL_ELF_HOOK_H
