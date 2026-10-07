// Shared declarations for the generated dispatch tables.
//
// Everything in src/generated/ is produced by scripts/generate_layer.py and is
// compiled by exactly one translation unit: src/dispatch.cpp.

#pragma once

#include <vulkan/vulkan.h>

#include "generated/dispatch_structs.h"

#if defined(__GNUC__) || defined(__clang__)
#define VKST_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#else
#define VKST_UNLIKELY(condition) (condition)
#endif

namespace vkst {

// Mask groups.  The bit positions must match the GROUPS list in
// scripts/generate_layer.py, which is what fills kCmdGroups[].
enum GroupBit : uint8_t {
    kGroupInstance = 0,
    kGroupDevice = 1,
    kGroupCreate = 2,
    kGroupQueue = 3,
    kGroupRecord = 4,
    kGroupCount = 5,
};

// One traced command: its Vulkan name, the trampoline that implements it, and
// which table the trampoline lives in.
struct Entry {
    const char* name;
    PFN_vkVoidFunction fn;
    bool instance_level;
};

extern const Entry kEntries[];
extern const uint32_t kEntryCount;

extern const uint32_t kCommandCount;
extern const char* const kCmdNames[];
extern const uint8_t kCmdGroups[];

void InitInstanceTable(InstanceTable* table, uintptr_t key, VkInstance instance,
                       PFN_vkGetInstanceProcAddr nextGIPA,
                       PFN_vkGetDeviceProcAddr nextGDPA);

void InitDeviceTable(DeviceTable* table, uintptr_t key, VkDevice device,
                     PFN_vkGetInstanceProcAddr nextGIPA,
                     PFN_vkGetDeviceProcAddr nextGDPA);

// Binary search over kEntries, which the generator emits in name order.
const Entry* FindEntry(const char* name);

}  // namespace vkst
