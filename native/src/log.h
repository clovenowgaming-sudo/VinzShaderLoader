#ifndef VSL_LOG_H
#define VSL_LOG_H

#include <android/log.h>

#define VSL_TAG "VinzShader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  VSL_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  VSL_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VSL_TAG, __VA_ARGS__)

#endif // VSL_LOG_H
