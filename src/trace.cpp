#include "trace.h"

#include <fnmatch.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "dispatch.h"
#include "log.h"

namespace vkst {

// Declared with external linkage in trace.h, and read by every generated
// trampoline, so it cannot live in the anonymous namespace below.
uint8_t g_enabled[kMaxCommands] = {};

namespace {

constexpr char kPropMask[] = "debug.vk.systrace.mask";
constexpr char kPropInclude[] = "debug.vk.systrace.include";
constexpr char kPropExclude[] = "debug.vk.systrace.exclude";
constexpr char kPropReload[] = "debug.vk.systrace.reload";

// Commands between two polls of the reload property.  A property read is far
// too expensive to do per call, and this is short enough that a setprop takes
// effect while the app is still running.
constexpr uint32_t kReloadInterval = 1024;

constexpr size_t kMaxPatterns = 32;
constexpr size_t kMaxPatternLength = 64;

constexpr uint32_t kDefaultMask = (1u << kGroupCount) - 1;

struct PatternList {
    size_t count = 0;
    char patterns[kMaxPatterns][kMaxPatternLength];
};

struct Config {
    uint32_t mask = kDefaultMask;
    PatternList include;
    PatternList exclude;
    char include_text[PROP_VALUE_MAX] = {};
    char exclude_text[PROP_VALUE_MAX] = {};
};

std::atomic<uint32_t> g_calls{0};
std::atomic<bool> g_reload_lock{false};

// Hash of the last value of debug.vk.systrace.reload that was applied.  A hash
// rather than the string itself, so that the hot path can compare it without
// sharing mutable memory with a concurrent reload.  The reload property is only
// ever written by hand, so the probability of two values colliding is not worth
// protecting against.
std::atomic<uint32_t> g_reload_token{0};
bool g_loaded = false;

uint32_t Fnv1a(const char* text) {
    uint32_t hash = 2166136261u;
    for (; *text != '\0'; ++text) {
        hash ^= static_cast<uint8_t>(*text);
        hash *= 16777619u;
    }
    return hash;
}

// Skip a reload if another thread is already applying one: the configuration
// being installed is no fresher than the one we would read.
class ReloadLock {
   public:
    ReloadLock() : held_(false) {
        bool expected = false;
        held_ = g_reload_lock.compare_exchange_strong(expected, true);
    }
    ~ReloadLock() {
        if (held_)
            g_reload_lock.store(false, std::memory_order_release);
    }
    bool held() const { return held_; }

    ReloadLock(const ReloadLock&) = delete;
    ReloadLock& operator=(const ReloadLock&) = delete;

   private:
    bool held_;
};

uint32_t ReadUintProperty(const char* name, uint32_t fallback) {
    char value[PROP_VALUE_MAX];
    if (__system_property_get(name, value) <= 0)
        return fallback;
    char* end = nullptr;
    const unsigned long parsed = strtoul(value, &end, 0);
    if (end == value)
        return fallback;
    return static_cast<uint32_t>(parsed);
}

// "vkCmdDraw*, vkQueueSubmit" -> two glob patterns.
void ParsePatterns(const char* property, PatternList* out, char* text) {
    out->count = 0;
    text[0] = '\0';
    if (__system_property_get(property, text) <= 0) {
        text[0] = '\0';
        return;
    }

    char* cursor = text;
    while (*cursor != '\0' && out->count < kMaxPatterns) {
        char* comma = strchr(cursor, ',');
        char* end = (comma != nullptr) ? comma : cursor + strlen(cursor);

        while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            ++cursor;
        while (end > cursor && (end[-1] == ' ' || end[-1] == '\t'))
            --end;

        const size_t length = static_cast<size_t>(end - cursor);
        if (length >= kMaxPatternLength) {
            ALOGW("pattern '%.*s' in %s is longer than %zu bytes, "
                  "ignored", static_cast<int>(length), cursor, property,
                  kMaxPatternLength - 1);
        } else if (length > 0) {
            memcpy(out->patterns[out->count], cursor, length);
            out->patterns[out->count][length] = '\0';
            ++out->count;
        }

        if (comma == nullptr)
            break;
        cursor = comma + 1;
    }
}

bool MatchesAny(const PatternList& list, const char* name) {
    for (size_t i = 0; i < list.count; ++i) {
        if (fnmatch(list.patterns[i], name, 0) == 0)
            return true;
    }
    return false;
}

void Apply(const Config& config, bool first_load) {
    uint32_t enabled = 0;
    for (uint32_t id = 0; id < kCommandCount; ++id) {
        const char* name = kCmdNames[id];

        bool on = ((config.mask >> kCmdGroups[id]) & 1u) != 0;
        if (on && config.include.count > 0)
            on = MatchesAny(config.include, name);
        if (on && config.exclude.count > 0)
            on = !MatchesAny(config.exclude, name);

        g_enabled[id] = on ? 1u : 0u;
        enabled += on ? 1u : 0u;
    }

    ALOGI("%s mask=0x%02x include='%s' exclude='%s' -> tracing "
          "%u of %u commands",
          first_load ? "loaded" : "reloaded", config.mask, config.include_text,
          config.exclude_text, enabled, kCommandCount);
}

}  // namespace

bool ReloadConfig() {
    ReloadLock lock;
    if (!lock.held())
        return false;

    Config config;
    config.mask = ReadUintProperty(kPropMask, kDefaultMask);
    ParsePatterns(kPropInclude, &config.include, config.include_text);
    ParsePatterns(kPropExclude, &config.exclude, config.exclude_text);

    const bool first_load = !g_loaded;
    g_loaded = true;
    Apply(config, first_load);
    return true;
}

void MaybeReload() {
    const uint32_t call = g_calls.fetch_add(1, std::memory_order_relaxed);
    if ((call % kReloadInterval) != 0)
        return;

    char token[PROP_VALUE_MAX];
    if (__system_property_get(kPropReload, token) <= 0)
        token[0] = '\0';

    const uint32_t hash = Fnv1a(token);
    if (g_loaded && hash == g_reload_token.load(std::memory_order_relaxed))
        return;

    if (ReloadConfig())
        g_reload_token.store(hash, std::memory_order_relaxed);
}

}  // namespace vkst
