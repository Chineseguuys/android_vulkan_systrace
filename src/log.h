// Logging.
//
// The NDK only ships <android/log.h>; the ALOG* macros used by AOSP come from
// the platform's log/log.h, which is not available here.

#pragma once

#include <android/log.h>

// Tag of every message this layer prints, so that they can be filtered with
// `adb logcat -s vk.systrace`.
#define VKST_TAG "vk.systrace"

#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, VKST_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, VKST_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, VKST_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, VKST_TAG, __VA_ARGS__)
