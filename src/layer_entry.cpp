// The entries of the layer that cannot be generated.
//
// The generated trampolines all follow the same shape: read the dispatch table
// out of the object that the command is called on and forward.  The commands
// here are different, because they are how the tables get created in the first
// place, or because the loader calls them before any table exists:
//
//   * vkGetInstanceProcAddr / vkGetDeviceProcAddr
//   * vkCreateInstance / vkDestroyInstance / vkCreateDevice / vkDestroyDevice
//   * the four vkEnumerate*Layer/ExtensionProperties entry points
//   * vkNegotiateLoaderLayerInterfaceVersion
//
// The loader contract implemented here was read from the AOSP sources
// (frameworks/native/vulkan/libvulkan):
//
//   layers_extensions.cpp  loads every libVkLayer*.so found on the layer search
//                          path, resolves vkGetInstanceProcAddr in it and calls
//                          vkEnumerateInstanceLayerProperties, then
//                          vkEnumerateDeviceLayerProperties, then
//                          vkEnumerateInstanceExtensionProperties(layerName)
//                          and, for a layer whose instance and device
//                          properties are identical,
//                          vkEnumerateDeviceExtensionProperties(NULL, layerName)
//   api.cpp                chains the layers by passing a
//                          VkLayerInstanceCreateInfo / VkLayerDeviceCreateInfo
//                          of function VK_LAYER_LINK_INFO in the pNext chain of
//                          vkCreateInstance / vkCreateDevice, filled with the
//                          pointer to the next layer's vkGet*ProcAddr
//   api_gen.cpp            fills the loader's own dispatch tables by calling our
//                          vkGetInstanceProcAddr (instance table) and our
//                          vkGetDeviceProcAddr (device table), and fails
//                          instance/device creation when a command it needs is
//                          missing

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cinttypes>
#include <cstdlib>
#include <cstring>

#include "dispatch.h"
#include "layer_state.h"
#include "log.h"
#include "trace.h"

namespace {

// ---------------------------------------------------------------------------
// Exported entry points
// ---------------------------------------------------------------------------

// The library is compiled with -fvisibility=hidden, which marks every symbol
// hidden already in the object file.  The linker leaves hidden symbols out of
// the dynamic symbol table, and a version script can only take symbols away,
// never add them back - so the seven entry points the loader looks up with
// dlsym() have to ask for default visibility themselves.  Everything else (the
// trampolines, the helpers, the C++ runtime) stays hidden, which is what keeps
// the exported surface at these seven names; export.map lists them as well.
#define VKST_EXPORT extern "C" __attribute__((visibility("default"))) VKAPI_ATTR

// ---------------------------------------------------------------------------
// Layer identity
// ---------------------------------------------------------------------------

// A char array member of an aggregate cannot be initialised from another array,
// so the name is written once as a macro and spelled out in both places.
#define VKST_LAYER_NAME "VK_LAYER_SYSTRACE_apitrace"

constexpr char kLayerName[] = VKST_LAYER_NAME;

// The loader memcmp()s the properties returned by the two enumerate functions
// to decide whether a layer belongs to the device chain as well
// (LayerLibrary::EnumerateLayers), so there is exactly one definition of them
// and both functions return a copy of it.
const VkLayerProperties kLayerProperties = {
    VKST_LAYER_NAME,
    VK_MAKE_VERSION(1, 0, 0),
    1,  // implementation version
    "Trace every Vulkan call as an ATrace/Perfetto slice",
};

// The Khronos headers spell the loader-link function VK_LAYER_LINK_INFO; the
// Android loader calls it VK_LAYER_FUNCTION_LINK.  Both are zero.
constexpr VkLayerFunction kLinkFunction = VK_LAYER_LINK_INFO;

// ---------------------------------------------------------------------------
// The loader's create info chains
// ---------------------------------------------------------------------------

// A pNext chain may hold anything, so it is walked with the two members that
// every Vulkan structure starts with (this is what the loader itself does in
// CreateInfoWrapper::SanitizePNext).
struct ChainHeader {
    VkStructureType sType;
    const void* pNext;
};

// Returns the loader's create info that carries the link to the layer below us,
// or nullptr if the chain has none.  It is returned mutable on purpose: the
// protocol requires every layer to advance u.pLayerInfo to the next link before
// it calls down, because the structure is shared by the whole chain and the
// layer below has to read its own link rather than ours (see "Example Code for
// CreateInstance" in the loader's LayerInterface doc).  There may be a second
// create info of the same type carrying the loader's data callback, hence the
// check on the function.
VkLayerInstanceCreateInfo* FindInstanceLink(const void* pNext) {
    for (const ChainHeader* header = static_cast<const ChainHeader*>(pNext);
         header != nullptr; header = static_cast<const ChainHeader*>(header->pNext)) {
        if (header->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)
            continue;
        // The const on pCreateInfo->pNext does not extend to these nodes: the
        // loader builds them for the layers to walk along.
        auto* info = const_cast<VkLayerInstanceCreateInfo*>(
            reinterpret_cast<const VkLayerInstanceCreateInfo*>(header));
        if (info->function == kLinkFunction && info->u.pLayerInfo != nullptr)
            return info;
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* FindDeviceLink(const void* pNext) {
    for (const ChainHeader* header = static_cast<const ChainHeader*>(pNext);
         header != nullptr; header = static_cast<const ChainHeader*>(header->pNext)) {
        if (header->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            continue;
        auto* info = const_cast<VkLayerDeviceCreateInfo*>(
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(header));
        if (info->function == kLinkFunction && info->u.pLayerInfo != nullptr)
            return info;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Handle casts
// ---------------------------------------------------------------------------

// vkGetInstanceProcAddr() is declared to take a VkInstance, but the loader
// resolves commands through it for whichever object it currently holds: an
// instance, a physical device and a device all carry a pointer to the same
// instance data in their second word (see layer_state.h), which is exactly what
// the loader's own driver::GetInstanceProcAddr() relies on.  Spelling the cast
// out here keeps the call sites honest about which handle they pass.
template <typename T>
inline PFN_vkVoidFunction InstanceProcAddr(PFN_vkGetInstanceProcAddr gipa, T object,
                                           const char* name) {
    return gipa(reinterpret_cast<VkInstance>(object), name);
}

// ---------------------------------------------------------------------------
// The commands implemented here rather than generated
// ---------------------------------------------------------------------------

// Commands that vkGetInstanceProcAddr must answer for a null instance.  These
// are the ones the loader asks for before any instance exists, plus the ones a
// caller may legitimately query without one.
PFN_vkVoidFunction GlobalEntry(const char* name) {
    if (strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceLayerProperties);
    if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceExtensionProperties);
    return nullptr;
}

// Commands implemented here that take an instance or a physical device.
PFN_vkVoidFunction InstanceEntry(const char* name) {
    // The loader asks for this one while it fills its own dispatch table
    // (InitDispatchTable in api_gen.cpp does INIT_PROC(true, instance,
    // GetInstanceProcAddr)), so it has to be answered with our own entry point
    // both with and without an instance.  Forwarding it to the layer below
    // would store the driver's function there instead, and anything the loader
    // then resolves through it would bypass the trampolines.
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance);
    if (strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
    if (strcmp(name, "vkEnumerateDeviceLayerProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceLayerProperties);
    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceExtensionProperties);
    // Not an instance command, but vkGetInstanceProcAddr is expected to hand
    // out the layer's own vkGetDeviceProcAddr for this name, for the same
    // reason.
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    return nullptr;
}

// Commands implemented here that need a device.  vkGetDeviceProcAddr must
// report these as device commands so that the loader fills the device table.
PFN_vkVoidFunction DeviceEntry(const char* name) {
    if (strcmp(name, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    return nullptr;
}

// True for command names that are known not to be device commands, so that
// vkGetDeviceProcAddr answers null for them instead of forwarding them and
// making the driver log an error.
bool IsKnownInstanceCommand(const char* name) {
    return GlobalEntry(name) != nullptr || InstanceEntry(name) != nullptr ||
           strcmp(name, "vkEnumerateInstanceVersion") == 0;
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

vkst::InstanceTable* AllocateInstanceTable() {
    return static_cast<vkst::InstanceTable*>(
        std::calloc(1, sizeof(vkst::InstanceTable)));
}

vkst::DeviceTable* AllocateDeviceTable() {
    return static_cast<vkst::DeviceTable*>(std::calloc(1, sizeof(vkst::DeviceTable)));
}

// Destroys an instance that could not be registered, so that a failure here
// does not leak it.
void DestroyUnregisteredInstance(VkInstance instance,
                                 const VkAllocationCallbacks* pAllocator,
                                 PFN_vkGetInstanceProcAddr next_gipa) {
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
        next_gipa(instance, "vkDestroyInstance"));
    if (destroy != nullptr)
        destroy(instance, pAllocator);
}

// Destroys a device that could not be registered.  The device entry point is
// what vkDestroyDevice is normally resolved through; asking the instance one is
// the fallback, and it has to be asked about the physical device because that
// is the handle that carries the instance's data.
void DestroyUnregisteredDevice(VkDevice device, VkPhysicalDevice physicalDevice,
                               const VkAllocationCallbacks* pAllocator,
                               PFN_vkGetInstanceProcAddr next_gipa,
                               PFN_vkGetDeviceProcAddr next_gdpa) {
    if (device == VK_NULL_HANDLE)
        return;

    PFN_vkDestroyDevice destroy = nullptr;
    if (next_gdpa != nullptr)
        destroy =
            reinterpret_cast<PFN_vkDestroyDevice>(next_gdpa(device, "vkDestroyDevice"));
    if (destroy == nullptr && next_gipa != nullptr)
        destroy = reinterpret_cast<PFN_vkDestroyDevice>(
            InstanceProcAddr(next_gipa, physicalDevice, "vkDestroyDevice"));

    if (destroy != nullptr)
        destroy(device, pAllocator);
}

}  // namespace

// ---------------------------------------------------------------------------
// Proc address entry points
// ---------------------------------------------------------------------------

VKST_EXPORT PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (pName == nullptr)
        return nullptr;

    if (instance == VK_NULL_HANDLE) {
        // Nothing has been chained yet, so the only commands we can answer for
        // are the ones that do not need a layer below us.  vkCreateInstance is
        // the important one: it is how the loader hands us the chain.
        return GlobalEntry(pName);
    }

    if (PFN_vkVoidFunction entry = InstanceEntry(pName))
        return entry;

    // Generated commands, instance and device level alike: the table they are
    // dispatched through is only known at call time, so the same trampoline
    // serves both.
    if (const vkst::Entry* entry = vkst::FindEntry(pName))
        return entry->fn;

    // Not ours: the layer below may still know it (driver commands and vendor
    // extensions).
    const vkst::InstanceTable* table = vkst::FindInstanceTable(vkst::KeyOf(instance));
    if (table != nullptr && table->nextGIPA != nullptr)
        return table->nextGIPA(instance, pName);

    return nullptr;
}

VKST_EXPORT PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (pName == nullptr || device == VK_NULL_HANDLE)
        return nullptr;

    if (PFN_vkVoidFunction entry = DeviceEntry(pName))
        return entry;

    if (const vkst::Entry* entry = vkst::FindEntry(pName)) {
        // The loader requires vkGetDeviceProcAddr to answer null for commands
        // that are not device commands.
        return entry->instance_level ? nullptr : entry->fn;
    }

    if (IsKnownInstanceCommand(pName))
        return nullptr;

    const vkst::DeviceTable* table = vkst::FindDeviceTable(vkst::KeyOf(device));
    if (table != nullptr && table->nextGDPA != nullptr)
        return table->nextGDPA(device, pName);

    return nullptr;
}

VKST_EXPORT VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    // The Android loader does not use this entry point: it resolves
    // vkGetInstanceProcAddr directly.  It is implemented for loaders that do
    // negotiate, and only accepts the revision this layer implements.
    if (pVersionStruct == nullptr ||
        pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        pVersionStruct->loaderLayerInterfaceVersion <
            MIN_SUPPORTED_LOADER_LAYER_INTERFACE_VERSION) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

VKST_EXPORT VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                   VkLayerProperties* pProperties) {
    if (pPropertyCount == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pProperties == nullptr) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }

    if (*pPropertyCount == 0)
        return VK_INCOMPLETE;

    *pPropertyCount = 1;
    pProperties[0] = kLayerProperties;
    return VK_SUCCESS;
}

// The loader queries this one with a null physical device and expects the same
// properties as the instance version.  Returning them is what makes the layer
// global, which is what puts device commands (vkCmd*, vkQueueSubmit) into the
// chain.
VKST_EXPORT VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                 uint32_t* pPropertyCount,
                                 VkLayerProperties* pProperties) {
    (void)physicalDevice;
    return vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VKST_EXPORT VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char* pLayerName,
                                       uint32_t* pPropertyCount,
                                       VkExtensionProperties* pProperties) {
    (void)pProperties;

    if (pPropertyCount == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pLayerName != nullptr && strcmp(pLayerName, kLayerName) != 0) {
        // Somebody else's layer: we have nothing to add to its list.  The
        // loader never asks us about another layer, but this keeps the entry
        // point honest.
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    // The layer itself exposes no extensions.
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKST_EXPORT VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                     const char* pLayerName,
                                     uint32_t* pPropertyCount,
                                     VkExtensionProperties* pProperties) {
    (void)pProperties;

    if (pPropertyCount == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pLayerName != nullptr) {
        // The loader asks with a null physical device and our own name while it
        // is discovering the layer; the answer has to be a success with an
        // empty list.
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    // A real query for the driver's device extensions.  The layer adds none of
    // its own, so the answer of the next layer down is passed through.
    const vkst::InstanceTable* table =
        vkst::FindInstanceTable(vkst::KeyOf(physicalDevice));
    if (table != nullptr && table->nextGIPA != nullptr) {
        const auto next = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            InstanceProcAddr(table->nextGIPA, physicalDevice,
                             "vkEnumerateDeviceExtensionProperties"));
        if (next != nullptr)
            return next(physicalDevice, pLayerName, pPropertyCount, pProperties);
    }

    *pPropertyCount = 0;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Instance and device lifecycle
// ---------------------------------------------------------------------------

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                 const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    if (pCreateInfo == nullptr || pInstance == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkLayerInstanceCreateInfo* chain = FindInstanceLink(pCreateInfo->pNext);
    if (chain == nullptr) {
        ALOGE("vkCreateInstance without loader link information, "
              "the layer cannot be used outside a loader chain");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkLayerInstanceLink* link = chain->u.pLayerInfo;
    PFN_vkGetInstanceProcAddr next_gipa = link->pfnNextGetInstanceProcAddr;
    if (next_gipa == nullptr) {
        ALOGE("vkCreateInstance without an instance proc address for the layer "
              "below");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto next = reinterpret_cast<PFN_vkCreateInstance>(
        next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (next == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Hand the next link to the layer below before calling into it.  Skipping
    // this would leave the next layer reading our link, and a second layer
    // would then resolve its own vkCreateInstance and recurse into itself.
    chain->u.pLayerInfo = link->pNext;

    const VkResult result = next(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS)
        return result;

    // The driver has already written its own instance pointer into the object,
    // so its key can be read now (see the comment in layer_state.h).
    const uintptr_t key = vkst::KeyOf(*pInstance);
    if (key == 0) {
        // Without that pointer there is no way to tell one object from another,
        // and every command would fail its table lookup with a confusing error
        // of its own, so say what is actually wrong.
        ALOGE("the loader left no instance data in %p, so the layer cannot map "
              "Vulkan objects to dispatch tables; tracing is off",
              reinterpret_cast<void*>(*pInstance));
    }

    vkst::InstanceTable* table = AllocateInstanceTable();
    if (table == nullptr) {
        DestroyUnregisteredInstance(*pInstance, pAllocator, next_gipa);
        *pInstance = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // The instance link only carries the instance entry point; the device one
    // has to be asked for, and is what vkCreateDevice will fall back to.
    const auto next_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        next_gipa(*pInstance, "vkGetDeviceProcAddr"));

    vkst::InitInstanceTable(table, key, *pInstance, next_gipa, next_gdpa);
    vkst::RegisterInstanceTable(table);

    // Reads the debug.vk.systrace.* properties and logs what is enabled.
    vkst::ReloadConfig();

    ALOGI("layer %s enabled for instance %p (key 0x%" PRIxPTR ")", kLayerName,
          reinterpret_cast<void*>(*pInstance), key);
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    if (instance == VK_NULL_HANDLE)
        return;

    vkst::InstanceTable* table = vkst::FindInstanceTable(vkst::KeyOf(instance));
    if (table == nullptr) {
        // Never created by us (or already destroyed): nothing to forward to.
        return;
    }

    const auto next = reinterpret_cast<PFN_vkDestroyInstance>(
        table->nextGIPA(instance, "vkDestroyInstance"));

    // Unregister before calling down: the driver is free to unmap the object,
    // and a concurrent call must not find a table that is about to go away.
    vkst::UnregisterInstanceTable(table);
    std::free(table);

    if (next != nullptr)
        next(instance, pAllocator);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice,
               const VkDeviceCreateInfo* pCreateInfo,
               const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    if (physicalDevice == VK_NULL_HANDLE || pCreateInfo == nullptr ||
        pDevice == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
    VkLayerDeviceCreateInfo* chain = FindDeviceLink(pCreateInfo->pNext);
    const VkLayerDeviceLink* link = nullptr;

    if (chain != nullptr) {
        link = chain->u.pLayerInfo;
        next_gipa = link->pfnNextGetInstanceProcAddr;
        next_gdpa = link->pfnNextGetDeviceProcAddr;
    }

    if (next_gipa == nullptr) {
        // No link information: fall back to the chain of the instance this
        // physical device belongs to.  Physical devices share the instance's
        // key.
        const vkst::InstanceTable* instance_table =
            vkst::FindInstanceTable(vkst::KeyOf(physicalDevice));
        if (instance_table == nullptr)
            return VK_ERROR_INITIALIZATION_FAILED;
        next_gipa = instance_table->nextGIPA;
        next_gdpa = instance_table->nextGDPA;
    }
    if (next_gipa == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    const auto next = reinterpret_cast<PFN_vkCreateDevice>(
        InstanceProcAddr(next_gipa, physicalDevice, "vkCreateDevice"));
    if (next == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Hand the next link to the layer below before calling into it, for the
    // same reason as in vkCreateInstance.
    if (link != nullptr)
        chain->u.pLayerInfo = link->pNext;

    const VkResult result = next(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS)
        return result;

    if (next_gdpa == nullptr) {
        next_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            InstanceProcAddr(next_gipa, *pDevice, "vkGetDeviceProcAddr"));
        if (next_gdpa == nullptr) {
            DestroyUnregisteredDevice(*pDevice, physicalDevice, pAllocator,
                                      next_gipa, nullptr);
            *pDevice = VK_NULL_HANDLE;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // The same key the device was created under has to be read back here: every
    // queue and command buffer of this device carries this pointer in turn, so
    // it is what routes vkQueueSubmit and vkCmd* to this table.
    const uintptr_t key = vkst::KeyOf(*pDevice);
    if (key == 0) {
        ALOGE("the loader left no device data in %p, so the layer cannot route "
              "queue and command buffer calls; tracing is off for this device",
              reinterpret_cast<void*>(*pDevice));
    }

    vkst::DeviceTable* table = AllocateDeviceTable();
    if (table == nullptr) {
        DestroyUnregisteredDevice(*pDevice, physicalDevice, pAllocator, next_gipa,
                                  next_gdpa);
        *pDevice = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Fills one slot per device command by asking the layer below, which is
    // also where the driver's own vkGetDeviceProcAddr comes in.
    vkst::InitDeviceTable(table, key, *pDevice, next_gipa, next_gdpa);
    vkst::RegisterDeviceTable(table);

    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    if (device == VK_NULL_HANDLE)
        return;

    vkst::DeviceTable* table = vkst::FindDeviceTable(vkst::KeyOf(device));
    if (table == nullptr)
        return;

    const auto next = reinterpret_cast<PFN_vkDestroyDevice>(
        table->nextGDPA != nullptr
            ? table->nextGDPA(device, "vkDestroyDevice")
            : nullptr);

    vkst::UnregisterDeviceTable(table);
    std::free(table);

    if (next != nullptr)
        next(device, pAllocator);
}
