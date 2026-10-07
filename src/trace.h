// Per-command tracing state.
//
// Every trampoline opens a TraceScope, which turns into an ATrace section only
// when the command is enabled by the current configuration and ATrace itself is
// running.  The configuration lives in the debug.vk.systrace.* system
// properties and is re-read when debug.vk.systrace.reload changes.

#pragma once

#include <android/trace.h>

#include <cstdint>

namespace vkst {

// Upper bound on the number of commands, so that the enable table can be a
// plain array indexed by the generator-assigned command id.  A build with more
// commands than this fails to compile (see the static_assert in dispatch.cpp).
constexpr uint32_t kMaxCommands = 1024;

// Zero or one per command; written when the configuration is loaded.
extern uint8_t g_enabled[kMaxCommands];

// Reads the properties and applies them.  Called once when an instance is
// created, and again whenever MaybeReload() notices a change.  Returns false if
// another thread was already applying a configuration.
bool ReloadConfig();

// Called by every trampoline.  Cheap enough for the hot path: it only polls the
// properties once every kReloadInterval commands.
void MaybeReload();

// An ATrace section for one Vulkan command.  Both the enable check and
// ATrace_isEnabled() are inlined into the trampoline, so a disabled command
// costs one byte load.
class TraceScope {
   public:
    inline TraceScope(const char* name, uint32_t id) {
        if (g_enabled[id] != 0 && ATrace_isEnabled()) {
            ATrace_beginSection(name);
            active_ = true;
        }
    }

    inline ~TraceScope() {
        if (active_)
            ATrace_endSection();
    }

    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

   private:
    bool active_ = false;
};

}  // namespace vkst
