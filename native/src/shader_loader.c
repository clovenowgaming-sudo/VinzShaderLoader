#include "shader_loader.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Fullscreen-triangle vertex shader. Shaderpacks only need to supply
 * final.fsh (the fragment pass) — this project only supports a single
 * post-process pass, so there's nothing stage-specific for the vertex
 * shader to do beyond handing the fragment shader a UV coordinate.
 */
static const char *FULLSCREEN_VSH =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 texcoord;\n"
    "void main() {\n"
    "    texcoord = a_texcoord;\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static char *read_file(const char *path, long *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read_n = fread(buf, 1, len, f);
    fclose(f);
    buf[read_n] = '\0';
    if (len_out) *len_out = (long)read_n;
    return buf;
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        char *log = (char *)malloc(log_len > 0 ? log_len : 256);
        glGetShaderInfoLog(shader, log_len > 0 ? log_len : 256, NULL, log);
        LOGE("shader_loader: compile failed (%s): %s",
             type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

int vsl_load_shaderpack(const char *base_dir, const char *pack_name, vsl_shader_program *out) {
    memset(out, 0, sizeof(*out));

    char frag_path[1024];
    snprintf(frag_path, sizeof(frag_path), "%s/%s/final.fsh", base_dir, pack_name);

    char *frag_src = read_file(frag_path, NULL);
    if (!frag_src) {
        LOGW("shader_loader: could not read %s (did you run shader_translate.py on this pack?)", frag_path);
        return -1;
    }

    char vert_path[1024];
    snprintf(vert_path, sizeof(vert_path), "%s/%s/post.vsh", base_dir, pack_name);
    char *vert_src = read_file(vert_path, NULL);
    const char *vert_to_use = vert_src ? vert_src : FULLSCREEN_VSH;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_to_use);
    GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, frag_src) : 0;

    free(frag_src);
    if (vert_src) free(vert_src);

    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return -2;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!linked) {
        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        char *log = (char *)malloc(log_len > 0 ? log_len : 256);
        glGetProgramInfoLog(program, log_len > 0 ? log_len : 256, NULL, log);
        LOGE("shader_loader: link failed for pack '%s': %s", pack_name, log);
        free(log);
        glDeleteProgram(program);
        return -3;
    }

    out->program = program;
    out->a_position   = glGetAttribLocation(program, "a_position");
    out->a_texcoord    = glGetAttribLocation(program, "a_texcoord");
    out->u_scene_color = glGetUniformLocation(program, "uSceneColor");
    out->u_scene_depth = glGetUniformLocation(program, "uSceneDepth");
    out->u_texel_size  = glGetUniformLocation(program, "uTexelSize");
    out->u_time        = glGetUniformLocation(program, "uTime");
    out->u_resolution  = glGetUniformLocation(program, "uResolution");
    out->valid = 1;

    LOGI("shader_loader: loaded shaderpack '%s' (program %u)", pack_name, program);
    return 0;
}

void vsl_unload_shaderpack(vsl_shader_program *prog) {
    if (prog->program) glDeleteProgram(prog->program);
    memset(prog, 0, sizeof(*prog));
}
