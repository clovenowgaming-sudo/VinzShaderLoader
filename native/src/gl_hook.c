#include "elf_hook.h"
#include "shader_loader.h"
#include "log.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <jni.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define VSL_TARGET_LIB "libminecraftpe.so"
#define VSL_SHADERPACK_BASE "/sdcard/VinzShader/shaderpacks"
#define VSL_CONFIG_PATH "/sdcard/VinzShader/active_pack.txt"
#define VSL_DEFAULT_PACK "sample_translated"

typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static eglSwapBuffers_t real_eglSwapBuffers = NULL;

static int pipeline_ready = 0;
static GLint last_w = 0, last_h = 0;
static GLuint capture_tex = 0;
static GLuint dummy_depth_tex = 0; // 1x1 white, see README: no real depth on default FB
static GLuint quad_vbo = 0;
static vsl_shader_program active_program;
static struct timespec start_time;

static float now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double dt = (ts.tv_sec - start_time.tv_sec) + (ts.tv_nsec - start_time.tv_nsec) / 1e9;
    return (float)dt;
}

static char *read_active_pack_name(void) {
    FILE *f = fopen(VSL_CONFIG_PATH, "r");
    static char buf[256];
    if (!f) {
        strncpy(buf, VSL_DEFAULT_PACK, sizeof(buf) - 1);
        return buf;
    }
    if (!fgets(buf, sizeof(buf), f)) {
        strncpy(buf, VSL_DEFAULT_PACK, sizeof(buf) - 1);
    }
    fclose(f);
    // strip trailing newline
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    if (len == 0) strncpy(buf, VSL_DEFAULT_PACK, sizeof(buf) - 1);
    return buf;
}

static void init_pipeline_resources(int w, int h) {
    if (capture_tex) glDeleteTextures(1, &capture_tex);
    glGenTextures(1, &capture_tex);
    glBindTexture(GL_TEXTURE_2D, capture_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!dummy_depth_tex) {
        glGenTextures(1, &dummy_depth_tex);
        glBindTexture(GL_TEXTURE_2D, dummy_depth_tex);
        unsigned char white[4] = {255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    if (!quad_vbo) {
        // fullscreen triangle strip: x, y, u, v
        const float verts[] = {
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
        };
        glGenBuffers(1, &quad_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    }

    last_w = w;
    last_h = h;
}

static void ensure_pipeline(int w, int h) {
    if (!pipeline_ready) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        char *pack = read_active_pack_name();
        if (vsl_load_shaderpack(VSL_SHADERPACK_BASE, pack, &active_program) != 0) {
            LOGW("gl_hook: no shaderpack loaded, post-process disabled this session");
        }
        init_pipeline_resources(w, h);
        pipeline_ready = 1;
        return;
    }
    if (w != last_w || h != last_h) {
        init_pipeline_resources(w, h);
    }
}

static void run_post_process(int w, int h) {
    if (!active_program.valid) return;

    // Capture whatever the game just rendered into framebuffer 0.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, capture_tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, w, h, 0);

    // Save minimal state we touch, so the game's own renderer isn't
    // surprised next frame (old fixed-pipeline-era engines can be picky).
    GLboolean depth_test_was_on = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glViewport(0, 0, w, h);
    glUseProgram(active_program.program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, capture_tex);
    if (active_program.u_scene_color >= 0) glUniform1i(active_program.u_scene_color, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, dummy_depth_tex); // see README: no real depth buffer available
    if (active_program.u_scene_depth >= 0) glUniform1i(active_program.u_scene_depth, 1);

    if (active_program.u_texel_size >= 0) glUniform2f(active_program.u_texel_size, 1.0f / w, 1.0f / h);
    if (active_program.u_time >= 0) glUniform1f(active_program.u_time, now_seconds());
    if (active_program.u_resolution >= 0) glUniform2f(active_program.u_resolution, (float)w, (float)h);

    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    if (active_program.a_position >= 0) {
        glEnableVertexAttribArray(active_program.a_position);
        glVertexAttribPointer(active_program.a_position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    }
    if (active_program.a_texcoord >= 0) {
        glEnableVertexAttribArray(active_program.a_texcoord);
        glVertexAttribPointer(active_program.a_texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (active_program.a_position >= 0) glDisableVertexAttribArray(active_program.a_position);
    if (active_program.a_texcoord >= 0) glDisableVertexAttribArray(active_program.a_texcoord);

    if (depth_test_was_on) glEnable(GL_DEPTH_TEST);
    if (blend_was_on) glEnable(GL_BLEND);
}

static EGLBoolean hooked_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    if (w > 0 && h > 0) {
        ensure_pipeline(w, h);
        run_post_process(w, h);
    }

    return real_eglSwapBuffers(dpy, surf);
}

static int vsl_install_hooks(void) {
    vsl_hook_request req = {
        .lib_name_substr = VSL_TARGET_LIB,
        .symbol_name = "eglSwapBuffers",
        .replacement = (void *)hooked_eglSwapBuffers,
        .original_out = (void **)&real_eglSwapBuffers,
    };

    int rc = vsl_hook_symbol(&req);
    if (rc != 0) {
        LOGE("gl_hook: failed to hook eglSwapBuffers (rc=%d) — post-process shaders will not run", rc);
        return rc;
    }

    LOGI("gl_hook: hook installed, post-process pipeline armed");
    return 0;
}

/*
 * JNI_OnLoad fires automatically the moment System.loadLibrary("vinzshader")
 * runs — no Java-side class or method needed. The only requirement is
 * load ORDER: this must happen AFTER libminecraftpe.so is already mapped,
 * so vsl_hook_symbol() can find its GOT. patch_apk.sh enforces this by
 * inserting our loadLibrary call in smali directly after the game's own
 * existing System.loadLibrary("minecraftpe") call — see patch/README.md.
 */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)vm; (void)reserved;
    vsl_install_hooks();
    return JNI_VERSION_1_6;
}
