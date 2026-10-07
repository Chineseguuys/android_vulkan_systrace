# VK_LAYER_SYSTRACE_apitrace

[中文说明](README_CN.md)

A Vulkan layer for the Android loader that wraps every Vulkan command in an
ATrace section, so that each call shows up as a slice on the application's
thread track in Perfetto.

The deliverable is `libVkLayer_systrace.so`. There is no root, no APK change and
no Perfetto SDK dependency: the layer calls `ATrace_beginSection`, which the
platform's Perfetto integration already records.

```
     slice per Vulkan command, on the application's thread
     |
     v
  vkQueueSubmit  ############################################
  vkAcquireNextImageKHR ######
  vkCmdDrawIndexed           ####      ####      ####
  vkQueuePresentKHR                ######
```

Everything traced is one ordinary Vulkan command. The layer does not know
anything about frames or surfaces, so a slice is only as meaningful as the call
it names.

## Contents

- [Requirements](#requirements)
- [Build](#build)
- [Quick start](#quick-start)
- [Manual setup with adb](#manual-setup-with-adb)
- [Checking that it works](#checking-that-it-works)
- [Reading the trace](#reading-the-trace)
- [Reducing noise](#reducing-noise)
- [How it works](#how-it-works)
- [Regenerating the trampolines](#regenerating-the-trampolines)
- [What has been verified](#what-has-been-verified)
- [Putting the library in the APK](#putting-the-library-in-the-apk)
- [Known limitations](#known-limitations)

## Requirements

| Tool | Version used here |
| --- | --- |
| Android NDK | r28c (`28.2.13676358`), any r26+ should do |
| Vulkan headers | the ones inside the NDK (`vulkan_core.h`, `vulkan_android.h`) |
| Vulkan registry | `/usr/share/vulkan/registry/vk.xml`, only needed to regenerate |
| cmake, ninja | 3.20+ |
| python3 | 3.6+ |
| adb | for deploy and capture |

The build targets `android-24` and needs no platform source tree.

The application being traced must be **debuggable**. The loader only offers
layer discovery to a debuggable application, and both deploy paths below rely on
that.

## Build

```sh
scripts/build.sh                    # arm64-v8a, the usual case
scripts/build.sh --all-abis         # arm64-v8a and armeabi-v7a
scripts/build.sh --abi armeabi-v7a
scripts/build.sh --ndk /path/to/ndk
```

The script finds the NDK through `--ndk`, `$ANDROID_NDK_HOME`,
`$ANDROID_NDK_ROOT`, `$ANDROID_HOME/ndk/<newest>` or
`~/Android/Sdk/ndk/<newest>`, regenerates the trampolines, and builds with
cmake + ninja. The result is `build/<abi>/libVkLayer_systrace.so`, and the
script prints its exported symbols.

The library is around 490 KB stripped; the unstripped build is several
megabytes, all debug info. That debug info is worth keeping: the build-id
matches what is installed on the device, so `ndk-stack` can symbolise a crash in
the layer from the unstripped file.

CMake also writes `compile_commands.json` into the build tree, which is what
clangd and most editors read to resolve the NDK headers and the
cross-compilation flags (`--target`, `--sysroot`) that a source file cannot be
parsed without. Since the build trees are per-ABI and the tools look in the
source directory, `scripts/build.sh` links the one of the first ABI to
`compile_commands.json` at the top of the checkout.

## Quick start

```sh
scripts/build.sh
scripts/deploy.sh on com.example.game      # settings path, the default
# restart the application, then:
scripts/capture.sh com.example.game 10
```

That writes `trace-com.example.game-<timestamp>.pftrace`. Open it at
<https://ui.perfetto.dev>.

If `deploy.sh` reports that `run-as` failed, the package is not debuggable, or
you can use the other install path:

```sh
scripts/deploy.sh on com.example.game --prop
```

To undo either one:

```sh
scripts/deploy.sh off com.example.game
scripts/deploy.sh status                   # what is currently installed
```

Neither path affects an application that is already running: the loader reads
the layer list while the process creates its Vulkan instance. `deploy.sh`
force-stops the application for you, so start it again afterwards.

### The two install paths

| | `--settings` (default) | `--prop` |
| --- | --- | --- |
| Where the library goes | `/data/user/0/<package>/` via `run-as` | `/data/local/debug/vulkan/` |
| What enables it | the global settings `enable_gpu_debug_layers`, `gpu_debug_app`, `gpu_debug_layers` | the property `debug.vulkan.layers` |
| Survives a reboot | yes | no, the property has to be set again |
| Needs | a debuggable package | a debuggable package, and a `/data/local/debug/vulkan` the shell may write to |

The settings path is the one to prefer: it does not depend on the property being
readable from the application's process, and on Android 16 the property path is
unavailable anyway because `/data/local/debug` no longer exists and the shell
user may not create it. `deploy.sh` reports that rather than failing quietly.
Writing the library into the APK avoids the data-directory copy as well, see
[Putting the library in the APK](#putting-the-library-in-the-apk).

## Manual setup with adb

The same thing as `deploy.sh`, command by command, for when the script is not
what you want or something in it needs checking. This is the sequence that was
run on the device described under [On a device](#on-a-device). Set `package`
once and the rest can be pasted as is.

```sh
package=com.example.game
abi=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')   # arm64-v8a here
so=build/$abi/libVkLayer_systrace.so
```

**1. Copy the library to the phone.** Note that `adb push` cannot write into
`/data/user/0/<package>` directly: that directory belongs to the application's
uid and the shell user has no access to it. Two steps instead, through
`/data/local/tmp`, which the shell may write and the application may read:

```sh
adb push "$so" /data/local/tmp/libVkLayer_systrace.so
adb shell chmod 644 /data/local/tmp/libVkLayer_systrace.so
adb shell run-as "$package" cp /data/local/tmp/libVkLayer_systrace.so ./libVkLayer_systrace.so
adb shell rm -f /data/local/tmp/libVkLayer_systrace.so

# run-as starts in the application's own data directory, which is the directory
# the loader searches for this package.  Confirm it landed:
adb shell run-as "$package" ls -l ./libVkLayer_systrace.so
```

`run-as` only works for a debuggable application; `adb shell run-as "$package" id`
prints the application's uid when it works and fails otherwise. If it fails,
either make the application debuggable or build the library into the APK, see
[Putting the library in the APK](#putting-the-library-in-the-apk).

**2. Tell the loader to load it.** Three global settings, all writable from
`adb` without root. The name has to be the layer name from
`vkEnumerateInstanceLayerProperties`, which is what
`src/layer_entry.cpp` advertises:

```sh
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app "$package"
adb shell settings put global gpu_debug_layers VK_LAYER_SYSTRACE_apitrace

# read back; an unset global setting prints "null"
adb shell settings get global enable_gpu_debug_layers
adb shell settings get global gpu_debug_app
adb shell settings get global gpu_debug_layers
```

**3. Restart the application.** The layer list is read while the process creates
its Vulkan instance, so an already running application never sees the change:

```sh
adb shell am force-stop "$package"
adb shell monkey -p "$package" -c android.intent.category.LAUNCHER 1
```

**4. Confirm that the loader picked the layer up.** Two different log tags are
involved: the loader logs under `vulkan`, the layer under `vk.systrace`.

```sh
adb logcat -c
# ... start the application, then:
adb logcat -d | grep -E 'searching for layers|added global layer|Loaded layer'
adb logcat -d -s vk.systrace
```

What to expect:

```
D vulkan  : searching for layers in '/data/user/0/com.example.game'
D vulkan  : added global layer 'VK_LAYER_SYSTRACE_apitrace' from library '/data/user/0/com.example.game/libVkLayer_systrace.so'
I vulkan  : Loaded layer VK_LAYER_SYSTRACE_apitrace
I vk.systrace: loaded mask=0x1f include='' exclude='' -> tracing 615 of 615 commands
I vk.systrace: layer VK_LAYER_SYSTRACE_apitrace enabled for instance 0x... (key 0x...)
```

If the `searching for layers` lines are missing entirely, the application is not
debuggable or was not restarted. If the path is searched but no
`added global layer` line appears, the library does not match `libVkLayer*.so`
or is not where the loader looks.

**5. Trace it.** `atrace_apps` has to name the process, see
[Checking that it works](#checking-that-it-works):

```sh
scripts/capture.sh "$package" 10
```

**6. Undo.** Remove the settings and the copied library:

```sh
adb shell settings delete global gpu_debug_layers
adb shell settings delete global gpu_debug_app
adb shell settings delete global enable_gpu_debug_layers
adb shell run-as "$package" rm -f ./libVkLayer_systrace.so
adb shell am force-stop "$package"
```

The property path is the same thing with a different middle step. It needs a
`/data/local/debug/vulkan` the shell may write to, which recent releases do not
provide:

```sh
adb shell setprop debug.vulkan.layers VK_LAYER_SYSTRACE_apitrace
adb shell getprop debug.vulkan.layers
# undo
adb shell setprop debug.vulkan.layers '""'
```

## Checking that it works

```sh
adb logcat -c && adb logcat -s vk.systrace
```

Start the application. Two lines mean the layer is live:

```
layer VK_LAYER_SYSTRACE_apitrace enabled for instance 0x7f8a1c0d20
loaded mask=0x1f include='' exclude='' -> tracing 615 of 615 commands
```

The first comes from `vkCreateInstance`, the second reports the configuration
that the `debug.vk.systrace.*` properties produced. If the first line is there
but no slices appear in the trace, the second line is what to look at: it says
how many commands are switched on.

Both milestones from the plan are covered by this: the layer being loaded (that
first line, plus no `VK_ERROR_LAYER_NOT_PRESENT` in the application) and a
`vkQueueSubmit` slice on the application's thread track.

If nothing appears at all, in order of likelihood:

1. The application is not debuggable, so the loader never looked for layers.
2. The application was already running when the layer was installed, so it
   never created a new instance.
3. The trace was recorded for the wrong process: `atrace_apps` in the capture
   config must match the process command line exactly. This is required, see
   below.
4. `ATrace_isEnabled()` was false for the process, which is the same condition
   as 3.

## Reading the trace

Look at the thread track of the application. Each Vulkan call is a slice named
after the command, nested the way the calls are nested: a `vkCmd*` recorded
inside `vkBeginCommandBuffer`/`vkEndCommandBuffer` appears between them, and
`vkQueueSubmit` appears after them.

Slices come from `ATrace_beginSection`/`ATrace_endSection` around the call, so
the duration is the time spent in the layer plus the time spent in the call
below it (the driver). A long `vkQueueSubmit` means the time was spent
somewhere inside the driver, not that the layer is slow.

The layer's own overhead is small but not zero, and it is not visible as a
separate slice: a slice covers the whole call, so its length is the layer's work
plus the driver's. `debug.vk.systrace.exclude` is the way to measure the
difference if it ever matters.

## Reducing noise

Every enabled command costs one property-free branch, and one
`ATrace_isEnabled()` plus two `ATrace_*Section` calls when it is enabled.
Tracing all 615 commands is fine for a start-up trace and painful for a
30-second gameplay capture, so the commands can be filtered while the
application runs.

The configuration lives in three system properties, read when the instance is
created and again whenever `debug.vk.systrace.reload` changes:

| Property | Meaning |
| --- | --- |
| `debug.vk.systrace.mask` | bitmask of the command groups, default `0x1f` (all) |
| `debug.vk.systrace.include` | comma separated `fnmatch` patterns; when set, only matching commands are traced |
| `debug.vk.systrace.exclude` | comma separated `fnmatch` patterns; matching commands are not traced |
| `debug.vk.systrace.reload` | any change to this value makes the layer re-read the three above |

The mask bits are:

| Bit | Group | Commands | What it covers |
| --- | --- | --- | --- |
| 0 | `instance` | 68 | instance and physical device queries |
| 1 | `device` | 178 | everything else at device level |
| 2 | `create` | 99 | `vkCreate*`/`vkDestroy*`/`vkAllocate*`/`vkFree*` |
| 3 | `queue` | 24 | `vkQueueSubmit`, `vkQueuePresentKHR`, waits and fences |
| 4 | `record` | 246 | `vkCmd*`, the command buffer recording calls |

`include` and `exclude` are applied after the mask, and both accept `*` and `?`,
where `*` also matches nothing, so `vkCmdDraw*` covers `vkCmdDraw` as well.

Recipes:

```sh
# Submit and present only, which shows the frame boundaries
adb shell setprop debug.vk.systrace.mask 0x08
adb shell setprop debug.vk.systrace.include 'vkQueueSubmit,vkQueueSubmit2,vkQueuePresentKHR,vkAcquireNextImage*'

# Everything except command buffer recording, which is 246 of the 615
adb shell setprop debug.vk.systrace.exclude 'vkCmd*'

# Only the commands that create and destroy objects
adb shell setprop debug.vk.systrace.mask 0x04
adb shell setprop debug.vk.systrace.include ''

# Apply the current properties without restarting
adb shell setprop debug.vk.systrace.reload 1
```

Setting `include` to the empty string goes back to "everything the mask allows".
Each change is logged:

```
reloaded mask=0x08 include='vkQueueSubmit,vkQueueSubmit2,...' exclude='' -> tracing 4 of 615 commands
```

The reload property is polled once every 1024 commands rather than per call, so
a change takes effect after a moment of activity rather than instantly.

## How it works

Three things are worth knowing if you are going to change the code.

**One trampoline per command, generated.** `scripts/generate_layer.py` reads
`vk.xml` and emits `src/generated/{dispatch_structs.h,trampolines.inc,tables.inc,entry_points.inc}`:
one forwarder per command, which opens a `TraceScope` and calls the next layer
down. The generated files are committed, so a build does not need the registry.

**Objects are mapped to dispatch tables through the loader's own cookie.** The
Android loader stores a pointer to its `InstanceData`/`DeviceData` in every
dispatchable object it creates, for instances, physical devices, devices, queues
and command buffers alike. The layer therefore keys its tables on that pointer
(`KeyOf()` in `src/layer_state.h`), and an instance and all of its physical
devices share one key while a device and all of its queues and command buffers
share another. That is why the layer never has to intercept
`vkCreateCommandPool`, `vkAllocateCommandBuffers`, `vkGetDeviceQueue` and
friends: any handle reached through those already resolves to the right table.

The pointer is in the object's **first** word, which is worth spelling out
because `driver.h` on its own suggests otherwise. `hardware/hwvulkan.h` defines
the cookie as a union:

```c
typedef union { uintptr_t magic; const void* vtbl; } hwvulkan_dispatch_t;
```

so `magic` and `vtbl` are the same storage: the HAL writes the magic when it
creates an object, and `SetData()` then overwrites that word with its own data
pointer. Keying on the second word instead is self-consistent for an instance,
because the layer writes and reads that key itself, which is exactly why such a
mistake survives a casual test and then fails on the first physical-device
query.

**The chain has to be advanced.** The loader hands each layer a
`VkLayerInstanceCreateInfo`/`VkLayerDeviceCreateInfo` of function
`VK_LAYER_LINK_INFO` in the `pNext` chain of `vkCreateInstance`/`vkCreateDevice`,
holding the `vkGet*ProcAddr` of the layer below. That structure is shared by the
whole chain, so a layer must set `u.pLayerInfo = u.pLayerInfo->pNext` before it
calls down, or the next layer reads *its* link and resolves its own entry point.
This is documented in the loader's `LoaderLayerInterface.md`, under "Example
Code for CreateInstance".

### Files

| Path | What it is |
| --- | --- |
| `src/layer_entry.cpp` | the eleven entry points that are not generated, including the exported seven |
| `src/layer_state.{h,cpp}` | handle key, instance/device table registry |
| `src/dispatch.{h,cpp}` | compiles the generated tables and trampolines |
| `src/trace.{h,cpp}` | `TraceScope` and the `debug.vk.systrace.*` configuration |
| `src/generated/` | generated, committed |
| `export.map` | the seven exported symbols |
| `scripts/` | build, deploy, capture, generator |

## Regenerating the trampolines

```sh
ANDROID_NDK_HOME=/path/to/ndk python3 scripts/generate_layer.py
python3 scripts/generate_layer.py --vk-xml /path/to/vk.xml --print-skipped
python3 scripts/generate_layer.py --allow-signature-mismatch   # see below
```

`--vk-xml`, or `$VK_XML`, is the **Vulkan API Registry**: the normative,
machine-readable definition of the API that Khronos maintains as part of the
specification, so it is the authority on which commands exist and what their
signatures are. There is nothing this repository has to keep in sync by hand.

Where to get it:

| Source | Notes |
| --- | --- |
| `/usr/share/vulkan/registry/vk.xml` | what the default points at; installed by the `vulkan-headers` distribution package (`1:1.4.357.0-1` on the machine this was written on, which is where "1.4.357" below comes from) |
| `git clone https://github.com/KhronosGroup/Vulkan-Docs`, then `xml/vk.xml` | upstream, always current |
| the LunarG Vulkan SDK, under `share/vulkan/registry/` | if you already have the SDK |
| `<ndk>/sources/third_party/vulkan/src/registry/vk.xml` | only in NDK 26.x, and it is registry 1.3.237 there; NDK 27 dropped it. Too old to be worth using |

The registry does not have to match the NDK headers, and in practice it will not:
it is normally much newer, which is what the filter below is for. Nothing breaks
when it is, and a newer registry is what lets a newer command be picked up the
moment the headers have it.

The generator only emits a command if `PFN_vk<name>` exists in the NDK's
Vulkan headers, because the trampoline has to be assignable to that type. The
registry is normally much newer than the NDK headers (here 1.4.357 against
1.3.275), which is what the filter is for: `--print-skipped` lists what was
left out, and bootstrapping a new NDK is a matter of re-running the script.

Two checks run before anything is written, because a trampoline whose signature
is wrong compiles through a `reinterpret_cast` and then misinterprets arguments
at run time:

- every emitted signature is compared against the header's `PFN_vk<name>`
  typedef, argument by argument, and generation stops on a mismatch;
- every type named by an emitted trampoline has to be declared by the headers.

## What has been verified

Locally, on the machine this was written on:

- Cross-compiles for `arm64-v8a` and `armeabi-v7a` with NDK r28c, warning free
  under `-Wall -Wextra`.
- The library exports exactly the seven symbols the loader looks up, and
  `scripts/build.sh` fails the build if any of them is missing. (This is not
  theory: without an explicit default visibility on those entry points,
  `-fvisibility=hidden` makes the linker drop them, and `--gc-sections` then
  deletes every trampoline.)
- 615 commands are generated and present in the binary, plus the eleven
  hand-written entry points.
- The generator is deterministic, its output is byte-sorted and free of
  duplicates (the trampoline lookup is a binary search over it), and the three
  checks it runs — header signature agreement, declared types, and the loader's
  required commands — pass.

### Against the loader source

The layer's contract with the loader was checked against the AOSP sources of
`frameworks/native/vulkan/libvulkan` (`api.cpp`, `api_gen.cpp`, `driver.cpp`,
`driver.h`, `layers_extensions.cpp`). Those checks are what the code comments
refer to, and each of them is a hard requirement rather than a detail:

| Checked in the loader | What the layer does about it |
| --- | --- |
| `hwvulkan_dispatch_t` is a **union** holding the loader's data in its first word, and `SetData()` is called for instances, physical devices, devices, queues and command buffers alike | `KeyOf()` reads that word as the table key, which is why queues and command buffers need no interception |
| `SetData()` runs inside `driver::CreateInstance`/`CreateDevice`, i.e. below the last layer | The key is read after the call down, so it is always populated |
| `LayerChain::SetupLayerLinks()` builds a linked list of `VkLayerInstanceLink`/`VkLayerDeviceLink`, with the last layer's `pNext` null and its next-proc pointing at `driver::Get*ProcAddr` | The link is advanced to `pNext` before every call down |
| `LayerChain::ModifyCreateInfo()` puts *two* `VkLayer*CreateInfo` structs in `pNext`: one of function `VK_LAYER_FUNCTION_LINK` and one carrying the data callback | The chain walk matches on the function, not just the `sType` |
| `InitDispatchTable()` stores `INIT_PROC(true, instance, GetInstanceProcAddr)` and `INIT_PROC(true, dev, GetDeviceProcAddr)`, failing creation when either is null | Both `vkGet*ProcAddr` answer for themselves rather than forwarding |
| `InitDispatchTable()` marks 12 instance and 121 device commands as required | `scripts/generate_layer.py` checks that each lands in the table the loader asks for it in, and stops if not |
| `layers_extensions.cpp` sets `layer.is_global = true` only when `memcmp()` of the properties from the two enumerate functions is zero | Both return a copy of one constant, which is also what puts `vkCmd*`/`vkQueueSubmit` into the chain |
| Layer discovery calls the enumerate functions with a null handle and expects a successful, empty answer | All four return `VK_SUCCESS` without dereferencing any handle |

Still open after the device run:

- Property access control: whether an application process may read
  `debug.vk.systrace.*` at all, which is what the filtering in
  [Reducing noise](#reducing-noise) depends on. The default configuration does
  not need it. See [Known limitations](#known-limitations).

### On a device

Run on an OPD2401 (Android 16, `ro.build.version.sdk=36`, `ro.build.type=user`,
arm64-v8a, no root) with the settings path and a debuggable application:

- The loader enumerates the layer and reports it global:

  ```
  D vulkan: searching for layers in '/data/user/0/<package>'
  D vulkan: added global layer 'VK_LAYER_SYSTRACE_apitrace' from library
            '/data/user/0/<package>/libVkLayer_systrace.so'
  I vulkan: Loaded layer VK_LAYER_SYSTRACE_apitrace
  ```

  The three paths it walked were the application's data directory, its
  `nativeLibraryDir` and `<base.apk>!/lib/arm64-v8a`.

- The layer installs its tables and traces everything:

  ```
  I vk.systrace: loaded mask=0x1f include='' exclude='' -> tracing 615 of 615 commands
  I vk.systrace: layer VK_LAYER_SYSTRACE_apitrace enabled for instance 0x... (key 0x...)
  ```

- A Perfetto capture of the sampled application contains 64 distinct Vulkan
  commands as slices, including `vkQueueSubmit`, `vkQueuePresentKHR`,
  `vkAcquireNextImageKHR`, `vkBeginCommandBuffer`/`vkEndCommandBuffer`,
  `vkCmdBeginRenderPass`/`vkCmdEndRenderPass`, `vkCmdDrawIndexed`,
  `vkCmdBindPipeline` and `vkCmdPushConstants`, next to the platform's own
  `dequeueBuffer`/`queueBuffer` slices. Device-level commands being present is
  what shows the layer is in the device chain, i.e. that `is_global` worked.

- The property path (`--prop`) does **not** work on this device: `/data/local/debug`
  does not exist and the shell user may not create it. `scripts/deploy.sh`
  now reports that instead of leaving the property set and the library absent.
  See [Putting the library in the APK](#putting-the-library-in-the-apk) for the
  way to avoid the data-directory copy altogether.

## Putting the library in the APK

Copying the library into the application's data directory is what `deploy.sh`
does by default, and it needs nothing but a debuggable build. To avoid that copy
entirely, the layer can ship inside the APK, because the loader searches the
application's APKs as archives: `LoadedApk` adds `<apk>!/lib/<abi>` to the
library search path, and the loader's `ForEachFileInPath` understands exactly
that `zip!/dir` syntax. The log above shows it happening.

Put `libVkLayer_systrace.so` in the application's native library directory for
the target ABI:

```
app/src/main/jniLibs/arm64-v8a/libVkLayer_systrace.so
```

and make sure the packaging does not compress it:

```groovy
android {
    packaging {
        jniLibs {
            // Same as android:extractNativeLibs="false" in the manifest.
            useLegacyPackaging = false
        }
    }
}
```

That is not cosmetic. The loader enumerates an archive entry only if

```cpp
if (entry.method != kCompressStored || entry.offset % kPageSize != 0)
    continue;   // ForEachFileInZip in layers_extensions.cpp
```

so the library has to be **stored uncompressed and page-aligned**; a compressed
or unaligned entry is skipped silently, and the layer simply never appears.
`useLegacyPackaging = false` is what makes AGP store and align native libraries
that way (page size is 4096 on the device this was tested on, 16384 on 16 KiB
page devices; the build already passes `-Wl,-z,max-page-size=16384` for those).

Enabling the layer does not change: it is still named by the settings, which
`adb` can write without root.

```sh
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app <package>
adb shell settings put global gpu_debug_layers VK_LAYER_SYSTRACE_apitrace
```

Confirm with `adb logcat -d | grep 'searching for layers'`: the library should
be reported as coming from `.../base.apk!/lib/<abi>`, not from the data
directory. If no `added global layer` line appears, the entry was compressed or
misaligned.

## Known limitations

- **The filter properties may not be readable.** `debug.vk.systrace.*` is read
  with `__system_property_get()`, and Android's property access control may deny
  that read to an application process. If it does, the layer sees the properties
  as unset and traces everything, which is the default anyway, and the start-up
  log line shows it. The fix, if it turns out to be needed, is to read the
  configuration from a file in the application's data directory instead, which
  the settings path already has write access to.
- `vkCreateInstance` with a null `pNext`, i.e. outside a loader chain, is
  refused with `VK_ERROR_INITIALIZATION_FAILED` rather than silently passing
  through. There is no useful way to trace outside a loader.
- Vendor extension commands that are missing from the registry, or present but
  not in the NDK headers, are forwarded without a slice (they are resolved
  through the next layer's `vkGet*ProcAddr`), and a command the driver does not
  implement returns an early fallback value with a one-off error log rather than
  crashing.
- One slice per call with no arguments recorded. Values of parameters, queue
  labels and GPU-side work are out of scope; this shows *which* calls an
  application makes and how long each one takes.
