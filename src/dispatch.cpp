// Compiles the generated dispatch tables and trampolines.
//
// This is the only translation unit that sees src/generated/*.inc; the rest of
// the layer talks to them through dispatch.h.

#include "dispatch.h"

#include <cstring>

#include "layer_state.h"
#include "trace.h"

// The generated files reference the tables and the tracing helpers, so they are
// included rather than linked.
#include "generated/tables.inc"
#include "generated/trampolines.inc"
#include "generated/entry_points.inc"

namespace vkst {

static_assert(kCommandCount <= kMaxCommands,
              "the enable table is smaller than the number of commands; raise "
              "kMaxCommands in src/trace.h");

const Entry* FindEntry(const char* name) {
    constexpr uint32_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);
    uint32_t low = 0;
    uint32_t high = kCount;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        const int order = strcmp(name, kEntries[middle].name);
        if (order == 0)
            return &kEntries[middle];
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return nullptr;
}

}  // namespace vkst
