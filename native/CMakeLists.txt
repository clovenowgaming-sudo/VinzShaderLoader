cmake_minimum_required(VERSION 3.10)
project(vinzshader C)

# Target matches the game's own ABI — PE-a0_15_10.apk ships armeabi-v7a
# only, no point building anything else for this specific target.
set(CMAKE_C_STANDARD 11)

add_library(vinzshader SHARED
    src/elf_hook.c
    src/gl_hook.c
    src/shader_loader.c
)

find_library(log-lib log)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)

target_link_libraries(vinzshader
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
)

target_compile_options(vinzshader PRIVATE -Wall -Wextra -O2 -fvisibility=hidden)

# Keep only JNI_OnLoad exported — everything else stays internal, less
# surface area for anything else in the process to stumble into.
set_target_properties(vinzshader PROPERTIES
    LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/vinzshader.map"
)
