#include "layer_state.h"

#include <cinttypes>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>

#include "log.h"
#include "trace.h"

namespace vkst {
namespace {

// A process creates one or two instances and a handful of devices; the arrays
// only have to survive pathological cases without allocating.
constexpr size_t kMaxInstances = 16;
constexpr size_t kMaxDevices = 16;

std::shared_mutex g_lock;
InstanceTable* g_instance_tables[kMaxInstances] = {};
DeviceTable* g_device_tables[kMaxDevices] = {};
DeviceTable* g_last_device = nullptr;

std::atomic<uint64_t> g_reported[(kMaxCommands + 63) / 64] = {};

}  // namespace

InstanceTable* FindInstanceTable(uintptr_t key) {
    if (key == 0)
        return nullptr;
    std::shared_lock<std::shared_mutex> guard(g_lock);
    for (InstanceTable* table : g_instance_tables) {
        if (table != nullptr && table->key == key)
            return table;
    }
    return nullptr;
}

DeviceTable* FindDeviceTable(uintptr_t key) {
    if (key == 0)
        return nullptr;
    std::shared_lock<std::shared_mutex> guard(g_lock);
    for (DeviceTable* table : g_device_tables) {
        if (table != nullptr && table->key == key)
            return table;
    }
    return nullptr;
}

DeviceTable* FallbackDeviceTable() {
    std::shared_lock<std::shared_mutex> guard(g_lock);
    return g_last_device;
}

void RegisterInstanceTable(InstanceTable* table) {
    std::unique_lock<std::shared_mutex> guard(g_lock);
    for (InstanceTable*& slot : g_instance_tables) {
        if (slot == nullptr) {
            slot = table;
            return;
        }
    }
    ALOGE("more than %zu instances, tracing is now incomplete",
          kMaxInstances);
}

void UnregisterInstanceTable(InstanceTable* table) {
    std::unique_lock<std::shared_mutex> guard(g_lock);
    for (InstanceTable*& slot : g_instance_tables) {
        if (slot == table) {
            slot = nullptr;
            return;
        }
    }
}

void RegisterDeviceTable(DeviceTable* table) {
    std::unique_lock<std::shared_mutex> guard(g_lock);
    for (DeviceTable*& slot : g_device_tables) {
        if (slot == nullptr) {
            slot = table;
            g_last_device = table;
            return;
        }
    }
    ALOGE("more than %zu devices, tracing is now incomplete",
          kMaxDevices);
}

void UnregisterDeviceTable(DeviceTable* table) {
    std::unique_lock<std::shared_mutex> guard(g_lock);
    for (DeviceTable*& slot : g_device_tables) {
        if (slot == table) {
            slot = nullptr;
            if (g_last_device == table)
                g_last_device = nullptr;
            return;
        }
    }
}

void ReportMissingEntry(uint32_t command_id, uintptr_t key, bool table_found) {
    const uint64_t bit = 1ull << (command_id & 63);
    std::atomic<uint64_t>& word = g_reported[command_id >> 6];
    if ((word.fetch_or(bit, std::memory_order_relaxed) & bit) != 0)
        return;  // already reported

    if (table_found) {
        ALOGE("the layer below has no entry point for %s, the call "
              "returns early",
              kCmdNames[command_id]);
        return;
    }

    if (key == kDispatchMagic) {
        ALOGE("%s was called on an object the loader has not taken over yet: "
              "its first word is still the HAL's magic 0x%" PRIxPTR,
              kCmdNames[command_id], key);
        return;
    }

    ALOGE("no dispatch table for %s: key 0x%" PRIxPTR " is not registered "
          "(the loader keeps a pointer to its own data in the first word of "
          "every object, and the layer keys its tables on it)",
          kCmdNames[command_id], key);
}

}  // namespace vkst
