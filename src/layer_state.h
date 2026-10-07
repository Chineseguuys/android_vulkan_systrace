// Mapping from Vulkan objects to the dispatch table for the layer below us.
//
// The Android loader stores a pointer to its own InstanceData/DeviceData in the
// second word of every dispatchable object it creates (see hwvulkan_dispatch_t
// in hardware/hwvulkan.h and SetData() in the loader's driver.h).  That word is
// therefore a stable key: the instance and every physical device share one, and
// a device and all of its queues and command buffers share another, which is why
// a layer does not have to intercept the creation of queues or command buffers.

#pragma once

#include <cstdint>

#include "dispatch.h"

namespace vkst {

// Key of any dispatchable handle.  Vulkan handles are pointers to incomplete
// types, so this only reads memory the loader owns.
//
// It is the *first* word, which is easy to get wrong from driver.h alone:
// hardware/hwvulkan.h defines the cookie as a union rather than a struct,
//
//     typedef union { uintptr_t magic; const void* vtbl; } hwvulkan_dispatch_t;
//
// so "magic" and "vtbl" live at the same offset.  The HAL writes the magic when
// it creates an object, and driver.h's SetData() then overwrites that very word
// with the pointer to the loader's InstanceData or DeviceData ("the loader will
// overwrite the vtbl field", as hwvulkan.h puts it).  Reading the second word
// instead still looks consistent for an instance, because the layer both writes
// and reads that key itself, but it misses physical devices, which carry a
// different value there.
//
// A null handle yields 0, which never matches a registered table.
template <typename T>
inline uintptr_t KeyOf(T object) {
    const void* pointer = reinterpret_cast<const void*>(object);
    if (pointer == nullptr)
        return 0;
    return reinterpret_cast<uintptr_t>(
        reinterpret_cast<const void* const*>(pointer)[0]);
}

// HWVULKAN_DISPATCH_MAGIC from hardware/hwvulkan.h.  This is what the first
// word holds after the HAL created the object and before the loader took it
// over, so finding it where a key is expected means SetData() has not run.
constexpr uintptr_t kDispatchMagic = 0x01CDC0DEu;

InstanceTable* FindInstanceTable(uintptr_t key);
DeviceTable* FindDeviceTable(uintptr_t key);

// Table of the most recently created device.  Only used by commands whose
// leading parameter is not dispatchable, which need a device table but have no
// object to derive one from.  No command in the current registry needs this,
// but the generator still emits the call for such commands.
DeviceTable* FallbackDeviceTable();

void RegisterInstanceTable(InstanceTable* table);
void UnregisterInstanceTable(InstanceTable* table);
void RegisterDeviceTable(DeviceTable* table);
void UnregisterDeviceTable(DeviceTable* table);

// Logs (once per command) that a trampoline could not forward a call, and why:
// either no dispatch table was found for the object the call was made on, or
// the table is there but the layer below has no entry point for the command.
// The first is a bug in this layer or a loader that stores its data somewhere
// unexpected; the second is normal for extension commands a driver does not
// implement.  Both used to be reported the same way, which made a broken key
// look like a missing driver entry point.
void ReportMissingEntry(uint32_t command_id, uintptr_t key, bool table_found);

}  // namespace vkst
