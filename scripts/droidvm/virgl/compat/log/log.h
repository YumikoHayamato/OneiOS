/* The part of AOSP's <log/log.h> that libvirglrenderer uses (src/mesa/util/os_misc.c: LOG_PRI), over the NDK's liblog. */
#pragma once
#include <android/log.h>
#ifndef LOG_PRI
#define LOG_PRI(priority, tag, ...) __android_log_print(priority, tag, __VA_ARGS__)
#endif
