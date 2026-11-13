#ifndef VSL_SHADER_LOADER_H
#define VSL_SHADER_LOADER_H

#include <GLES2/gl2.h>
#include <stdint.h>

typedef struct {
    GLuint program;
    GLint u_scene_color;
    GLint u_scene_depth;
    GLint u_texel_size;
    GLint u_time;
    GLint u_resolution;
    GLint a_position;
    GLint a_texcoord;
    int valid;
} vsl_shader_program;

/*
 * Loads <base_dir>/<pack_name>/pack.json + final.fsh (already translated
 * by tools/shader_translate.py — this loader does NOT translate, it just
 * compiles). If final.fsh is missing or fails to compile, returns a
 * program with valid=0 and the caller should skip post-processing that
 * frame (never crash the game over a bad shaderpack).
 */
int vsl_load_shaderpack(const char *base_dir, const char *pack_name, vsl_shader_program *out);

/* Frees GL resources held by a previously loaded program. */
void vsl_unload_shaderpack(vsl_shader_program *prog);

#endif // VSL_SHADER_LOADER_H
