cmake_minimum_required(VERSION 3.20)

project(VkLayer_systrace LANGUAGES CXX)

# Write compile_commands.json next to the build files: it is what clangd and
# most editors read to find the headers and to pick up the NDK's
# cross-compilation flags (--target, --sysroot), which a translation unit
# cannot be parsed without.  Only the Ninja and Makefile generators implement
# the option, and scripts/build.sh uses Ninja.
#
# The file lands in build/<abi>/, so scripts/build.sh also links it into the
# source directory, which is where the tools look by default.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A Vulkan layer for the Android loader: every Vulkan command that the platform
# headers know about is intercepted and wrapped in an ATrace slice, so that the
# calls show up on the application's thread track in Perfetto.
#
# src/generated/ holds the 600-odd trampolines; scripts/generate_layer.py
# produces them from the Khronos registry and the Vulkan headers of the NDK.

if(NOT ANDROID)
  message(FATAL_ERROR
    "This layer only supports the Android loader: it maps Vulkan objects to "
    "dispatch tables through the cookie that the Android loader stores in "
    "them. Configure with the NDK toolchain file instead.")
endif()

add_library(VkLayer_systrace SHARED
  src/dispatch.cpp
  src/layer_entry.cpp
  src/layer_state.cpp
  src/trace.cpp
)

target_compile_features(VkLayer_systrace PRIVATE cxx_std_17)

# vulkan.h only pulls in vulkan_android.h (vkCreateAndroidSurfaceKHR and the
# AHardwareBuffer commands) when the platform is selected; the platform headers
# are built with the same define.
target_compile_definitions(VkLayer_systrace PRIVATE
  VK_USE_PLATFORM_ANDROID_KHR=1
)

target_compile_options(VkLayer_systrace PRIVATE
  -O2
  -fvisibility=hidden
  -fvisibility-inlines-hidden
  -fno-exceptions
  -fno-rtti
  -ffunction-sections
  -fdata-sections
  -Wall
  -Wextra
  -Wno-unused-parameter
)

target_include_directories(VkLayer_systrace PRIVATE src)

# Linked in this configuration: --version-script keeps the exported surface at
# the seven symbols the loader looks for, and the 16 KiB page size keeps the
# library loadable on devices with 16 KiB pages.  The script is a link
# dependency so that editing it relinks.
set(VKST_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/export.map")
set_target_properties(VkLayer_systrace PROPERTIES
  LINK_DEPENDS "${VKST_VERSION_SCRIPT}"
)

target_link_options(VkLayer_systrace PRIVATE
  "-Wl,--version-script=${VKST_VERSION_SCRIPT}"
  "-Wl,--gc-sections"
  "-Wl,-z,max-page-size=16384"
)

# libandroid.so provides ATrace_beginSection/endSection/isEnabled, liblog.so the
# __android_log_print used by src/log.h.
target_link_libraries(VkLayer_systrace PRIVATE android log)
