# VK_LAYER_SYSTRACE_apitrace

[English](README.md)

一个面向 Android loader 的 Vulkan layer：把每一条 Vulkan 命令都包进一个 ATrace
区间，于是每次调用都会在 Perfetto 里以 slice 的形式出现在应用线程轨道上。

交付物是 `libVkLayer_systrace.so`。不需要 root，不需要改动 APK，也不依赖
Perfetto SDK：layer 调用的是 `ATrace_beginSection`，而平台的 Perfetto 集成本来
就会记录它。

```
     每条 Vulkan 命令一个 slice，位于应用线程上
     |
     v
  vkQueueSubmit  ############################################
  vkAcquireNextImageKHR ######
  vkCmdDrawIndexed           ####      ####      ####
  vkQueuePresentKHR                ######
```

被记录下来的每一样东西都是一条普通的 Vulkan 命令。layer 对帧、surface 一无所知，
所以一个 slice 的含义完全取决于它所标注的那次调用。

## 目录

- [环境要求](#环境要求)
- [构建](#构建)
- [快速开始](#快速开始)
- [手动 adb 步骤](#手动-adb-步骤)
- [确认是否生效](#确认是否生效)
- [阅读抓到的 trace](#阅读抓到的-trace)
- [降低噪声](#降低噪声)
- [实现原理](#实现原理)
- [重新生成 trampoline](#重新生成-trampoline)
- [已验证的内容](#已验证的内容)
- [把 so 集成进 APK](#把-so-集成进-apk)
- [已知限制](#已知限制)

## 环境要求

| 工具 | 本文使用的版本 |
| --- | --- |
| Android NDK | r28c（`28.2.13676358`），r26 及以上应该都可以 |
| Vulkan 头文件 | NDK 自带的那份（`vulkan_core.h`、`vulkan_android.h`） |
| Vulkan registry | `/usr/share/vulkan/registry/vk.xml`，只有重新生成时才需要 |
| cmake、ninja | 3.20+ |
| python3 | 3.6+ |
| adb | 用于部署与抓取 |

构建目标是 `android-24`，不需要平台源码树。

被跟踪的应用必须是 **debuggable** 的。loader 只对 debuggable 的应用做 layer
发现，下面两条注入路径都依赖这一点。

## 构建

```sh
scripts/build.sh                    # arm64-v8a，最常见的情况
scripts/build.sh --all-abis         # arm64-v8a 与 armeabi-v7a
scripts/build.sh --abi armeabi-v7a
scripts/build.sh --ndk /path/to/ndk
```

脚本通过 `--ndk`、`$ANDROID_NDK_HOME`、`$ANDROID_NDK_ROOT`、
`$ANDROID_HOME/ndk/<newest>` 或 `~/Android/Sdk/ndk/<newest>` 中的任意一个找到
NDK，重新生成 trampoline，然后用 cmake + ninja 构建。产物是
`build/<abi>/libVkLayer_systrace.so`，脚本会打印它导出的符号。

strip 之后库大约是 490 KB；未 strip 的构建有好几 MB，全是调试信息。这些调试信息
值得保留：它的 build-id 与设备上安装的那份一致，所以可以用未 strip 的文件让
`ndk-stack` 符号化 layer 里的崩溃。

CMake 还会把 `compile_commands.json` 写进构建目录。clangd 和大多数编辑器就是靠它
来解析 NDK 头文件、以及拿到交叉编译必需的编译选项（`--target`、`--sysroot`）——
缺了这些参数，源文件根本没法被正确解析。由于构建目录是按 ABI 分开的、而工具默认到
源码目录去找这个文件，`scripts/build.sh` 会把第一个 ABI 的那一份软链接到仓库根目录
的 `compile_commands.json`。

## 快速开始

```sh
scripts/build.sh
scripts/deploy.sh on com.example.game      # settings 路径，默认
# 重启应用，然后：
scripts/capture.sh com.example.game 10
```

这会写出 `trace-com.example.game-<timestamp>.pftrace`，用
<https://ui.perfetto.dev> 打开即可。

如果 `deploy.sh` 报告 `run-as` 失败，说明该包不是 debuggable 的；或者你也可以换
另一条注入路径：

```sh
scripts/deploy.sh on com.example.game --prop
```

撤销其中任意一条：

```sh
scripts/deploy.sh off com.example.game
scripts/deploy.sh status                   # 当前装了什么
```

两条路径都不会影响已经在运行的应用：loader 是在进程创建 Vulkan instance 的时候
读取 layer 列表的。`deploy.sh` 会替你 force-stop 应用，所以之后要重新启动它。

### 两条注入路径

| | `--settings`（默认） | `--prop` |
| --- | --- | --- |
| 库放在哪 | 通过 `run-as` 放到 `/data/user/0/<package>/` | `/data/local/debug/vulkan/` |
| 靠什么启用 | 三个全局 settings：`enable_gpu_debug_layers`、`gpu_debug_app`、`gpu_debug_layers` | 属性 `debug.vulkan.layers` |
| 重启后是否保留 | 是 | 否，属性需要重新设置 |
| 前提 | 包是 debuggable 的 | 包是 debuggable 的，且 shell 对 `/data/local/debug/vulkan` 有写权限 |

推荐用 settings 路径：它不依赖应用进程能否读取属性；而且在 Android 16 上属性路径
本来就不可用，因为 `/data/local/debug` 已经不存在、shell 用户也无权创建它。
`deploy.sh` 会明确报出这一点，而不是静默失败。把库打进 APK 同样可以免掉复制到
数据目录这一步，见[把 so 集成进 APK](#把-so-集成进-apk)。

## 手动 adb 步骤

下面就是 `deploy.sh` 所做的事，一条条列出来，供你不方便用脚本、或者需要核对脚本
里某一步时使用。这也是[在真实设备上](#在真实设备上)那台设备上实际跑过的顺序。
先设一次 `package`，后面可以直接整段粘贴。

```sh
package=com.example.game
abi=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')   # 这里是 arm64-v8a
so=build/$abi/libVkLayer_systrace.so
```

**1. 把库拷到手机上。** 注意 `adb push` 无法直接写进 `/data/user/0/<package>`：
那个目录属于应用的 uid，shell 用户没有权限。所以要走两步，借道
`/data/local/tmp`——shell 可写，应用可读：

```sh
adb push "$so" /data/local/tmp/libVkLayer_systrace.so
adb shell chmod 644 /data/local/tmp/libVkLayer_systrace.so
adb shell run-as "$package" cp /data/local/tmp/libVkLayer_systrace.so ./libVkLayer_systrace.so
adb shell rm -f /data/local/tmp/libVkLayer_systrace.so

# run-as 启动时的工作目录就是应用自己的数据目录，也正
# 是 loader 为该包搜索的目录。确认它到位了：
adb shell run-as "$package" ls -l ./libVkLayer_systrace.so
```

`run-as` 只对 debuggable 的应用有效；能工作时 `adb shell run-as "$package" id`
会打印应用的 uid，否则会失败。如果失败，要么把应用改成 debuggable，要么把库打进
APK，见[把 so 集成进 APK](#把-so-集成进-apk)。

**2. 让 loader 去加载它。** 三个全局 settings，全都可以从 `adb` 写入，不需要
root。这里的名字必须是 `vkEnumerateInstanceLayerProperties` 报告出来的 layer
名，也就是 `src/layer_entry.cpp` 所声明的那个：

```sh
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app "$package"
adb shell settings put global gpu_debug_layers VK_LAYER_SYSTRACE_apitrace

# 读回来核对；未设置的全局 setting 会打印 "null"
adb shell settings get global enable_gpu_debug_layers
adb shell settings get global gpu_debug_app
adb shell settings get global gpu_debug_layers
```

**3. 重启应用。** layer 列表是在进程创建 Vulkan instance 时读取的，所以已经在
运行的应用永远看不到这次改动：

```sh
adb shell am force-stop "$package"
adb shell monkey -p "$package" -c android.intent.category.LAUNCHER 1
```

**4. 确认 loader 已经把 layer 拿起来了。** 这里涉及两个不同的 log tag：loader 用
`vulkan`，layer 用 `vk.systrace`。

```sh
adb logcat -c
# ... 启动应用，然后：
adb logcat -d | grep -E 'searching for layers|added global layer|Loaded layer'
adb logcat -d -s vk.systrace
```

预期输出：

```
D vulkan  : searching for layers in '/data/user/0/com.example.game'
D vulkan  : added global layer 'VK_LAYER_SYSTRACE_apitrace' from library '/data/user/0/com.example.game/libVkLayer_systrace.so'
I vulkan  : Loaded layer VK_LAYER_SYSTRACE_apitrace
I vk.systrace: loaded mask=0x1f include='' exclude='' -> tracing 615 of 615 commands
I vk.systrace: layer VK_LAYER_SYSTRACE_apitrace enabled for instance 0x... (key 0x...)
```

如果连 `searching for layers` 都没有，说明应用不是 debuggable 的，或者没有重启。
如果路径被搜索了但没有 `added global layer` 那一行，说明库的文件名不匹配
`libVkLayer*.so`，或者不在 loader 会去找的位置。

**5. 抓 trace。** `atrace_apps` 必须填进程名，见[确认是否生效](#确认是否生效)：

```sh
scripts/capture.sh "$package" 10
```

**6. 撤销。** 删掉 settings 和拷进去的库：

```sh
adb shell settings delete global gpu_debug_layers
adb shell settings delete global gpu_debug_app
adb shell settings delete global enable_gpu_debug_layers
adb shell run-as "$package" rm -f ./libVkLayer_systrace.so
adb shell am force-stop "$package"
```

属性路径除了中间一步以外完全一样。它需要一个 shell 有写权限的
`/data/local/debug/vulkan`，而较新的发行版已经不提供：

```sh
adb shell setprop debug.vulkan.layers VK_LAYER_SYSTRACE_apitrace
adb shell getprop debug.vulkan.layers
# 撤销
adb shell setprop debug.vulkan.layers '""'
```

## 确认是否生效

```sh
adb logcat -c && adb logcat -s vk.systrace
```

启动应用。出现下面两行就说明 layer 活着：

```
layer VK_LAYER_SYSTRACE_apitrace enabled for instance 0x7f8a1c0d20
loaded mask=0x1f include='' exclude='' -> tracing 615 of 615 commands
```

第一行来自 `vkCreateInstance`，第二行报告 `debug.vk.systrace.*` 属性最终产生的
配置。如果第一行有、但 trace 里没有 slice，那么该看的就是第二行：它说明有多少条
命令被打开了。

计划里的两个里程碑都被这覆盖了：layer 被加载（第一行，加上应用侧不再出现
`VK_ERROR_LAYER_NOT_PRESENT`），以及应用线程轨道上出现 `vkQueueSubmit` slice。

如果什么都没有出现，按可能性排序：

1. 应用不是 debuggable 的，于是 loader 根本没去找 layer。
2. 装 layer 的时候应用已经在运行，于是它从未创建新的 instance。
3. trace 抓错了进程：抓取配置里的 `atrace_apps` 必须与进程命令行完全一致。这一点
   是必需的，见下。
4. 该进程的 `ATrace_isEnabled()` 为 false，这与第 3 点是同一个条件。

## 阅读抓到的 trace

看应用的线程轨道。每次 Vulkan 调用都是一个以命令名命名的 slice，嵌套关系与调用
关系一致：记录在 `vkBeginCommandBuffer`/`vkEndCommandBuffer` 之间的 `vkCmd*`
出现在这两者之间，`vkQueueSubmit` 出现在它们之后。

slice 来自调用前后的 `ATrace_beginSection`/`ATrace_endSection`，所以它的时长是
layer 里的时间加上它下面那次调用（驱动）的时间。`vkQueueSubmit` 很长意味着时间花
在驱动内部，而不是 layer 慢。

layer 自身的开销很小但不为零，而且它不会单独显示为一个 slice：一个 slice 覆盖整次
调用，长度是 layer 的工作量加上驱动的。真需要区分时，用
`debug.vk.systrace.exclude` 来量差值。

## 降低噪声

每条被打开的命令都有一次不带属性读取的分支判断；当它被打开时，再加一次
`ATrace_isEnabled()` 和两次 `ATrace_*Section` 调用。把 615 条命令全打开用来抓
启动阶段的 trace 没问题，用来抓 30 秒游戏过程就很痛苦，所以可以在应用运行期间对
命令做过滤。

配置放在三个系统属性里，在 instance 创建时读取一次，之后每当
`debug.vk.systrace.reload` 变化时再读一次：

| 属性 | 含义 |
| --- | --- |
| `debug.vk.systrace.mask` | 命令分组的位掩码，默认 `0x1f`（全部） |
| `debug.vk.systrace.include` | 逗号分隔的 `fnmatch` 模式；设置后只跟踪匹配的命令 |
| `debug.vk.systrace.exclude` | 逗号分隔的 `fnmatch` 模式；匹配的命令不被跟踪 |
| `debug.vk.systrace.reload` | 只要这个值一变，layer 就重新读取上面三个属性 |

掩码各位的含义：

| 位 | 分组 | 命令数 | 覆盖范围 |
| --- | --- | --- | --- |
| 0 | `instance` | 68 | instance 与 physical device 查询 |
| 1 | `device` | 178 | 其余 device 级命令 |
| 2 | `create` | 99 | `vkCreate*`/`vkDestroy*`/`vkAllocate*`/`vkFree*` |
| 3 | `queue` | 24 | `vkQueueSubmit`、`vkQueuePresentKHR`、等待与 fence |
| 4 | `record` | 246 | `vkCmd*`，命令缓冲区录制调用 |

`include` 与 `exclude` 在掩码之后生效，两者都支持 `*` 和 `?`，其中 `*` 也能匹配
空串，所以 `vkCmdDraw*` 同样覆盖 `vkCmdDraw`。

配方：

```sh
# 只看 submit 和 present，能看出帧边界
adb shell setprop debug.vk.systrace.mask 0x08
adb shell setprop debug.vk.systrace.include 'vkQueueSubmit,vkQueueSubmit2,vkQueuePresentKHR,vkAcquireNextImage*'

# 除了命令缓冲区录制以外的全部，录制占了 615 条里的 246 条
adb shell setprop debug.vk.systrace.exclude 'vkCmd*'

# 只看创建和销毁对象的那些命令
adb shell setprop debug.vk.systrace.mask 0x04
adb shell setprop debug.vk.systrace.include ''

# 不重启就应用当前属性
adb shell setprop debug.vk.systrace.reload 1
```

把 `include` 设为空串就回到“掩码允许的全部”。每次变更都会打日志：

```
reloaded mask=0x08 include='vkQueueSubmit,vkQueueSubmit2,...' exclude='' -> tracing 4 of 615 commands
```

reload 属性是每 1024 条命令轮询一次，而不是每次调用都读，所以改动会在有一点活动
之后生效，而不是立刻生效。

## 实现原理

如果你要改这份代码，有三件事值得知道。

**每条命令一个转发函数，由生成器产生。** `scripts/generate_layer.py` 读取
`vk.xml`，产出
`src/generated/{dispatch_structs.h,trampolines.inc,tables.inc,entry_points.inc}`：
每条命令一个转发器，它打开一个 `TraceScope`，然后调用下一层。生成物是提交入库的，
所以构建本身不需要 registry。

**对象到分发表的映射，用的是 loader 自己的 cookie。** Android loader 会把它自己的
`InstanceData`/`DeviceData` 指针写进它创建的每一个可分发对象里，instance、
physical device、device、queue 和 command buffer 都是如此。所以 layer 就用这个
指针作为表的 key（`src/layer_state.h` 里的 `KeyOf()`）：一个 instance 和它所有的
physical device 共用一个 key，一个 device 和它所有的 queue、command buffer 共用
另一个。这就是为什么 layer 从不需要拦截 `vkCreateCommandPool`、
`vkAllocateCommandBuffers`、`vkGetDeviceQueue` 之类：经由它们拿到的任何句柄，
本来就会解析到正确的表。

这个指针在对象的**第一个**字里，这一点值得专门写出来，因为单看 `driver.h` 很容易
得出别的结论。`hardware/hwvulkan.h` 把这个 cookie 定义成一个 union：

```c
typedef union { uintptr_t magic; const void* vtbl; } hwvulkan_dispatch_t;
```

也就是说 `magic` 和 `vtbl` 是同一块存储：HAL 创建对象时写入 magic，随后
`SetData()` 用loader 自己的数据指针覆盖掉同一个字。改用第二个字取 key，对
instance 而言是自洽的——因为那个 key 是 layer 自己写、自己读——而这恰恰说明为什么
这种错误能挺过一次粗略的测试，却在第一次 physical device 查询时就崩掉。

**链必须被推进。** loader 会在 `vkCreateInstance`/`vkCreateDevice` 的 `pNext` 链
里给每个 layer 放一个函数为 `VK_LAYER_LINK_INFO` 的
`VkLayerInstanceCreateInfo`/`VkLayerDeviceCreateInfo`，里面是下一层的
`vkGet*ProcAddr`。这个结构被整条链共享，所以 layer 在向下调用之前必须执行
`u.pLayerInfo = u.pLayerInfo->pNext`，否则下一层读到的是*它自己*的 link，会解析
到自己的入口点。这一点在 loader 的 `LoaderLayerInterface.md` 里的
"Example Code for CreateInstance" 一节有说明。

### 文件一览

| 路径 | 说明 |
| --- | --- |
| `src/layer_entry.cpp` | 十一个非生成的入口点，包含导出的那七个 |
| `src/layer_state.{h,cpp}` | 句柄 key、instance/device 表注册 |
| `src/dispatch.{h,cpp}` | 编译生成的分发表与 trampoline |
| `src/trace.{h,cpp}` | `TraceScope` 与 `debug.vk.systrace.*` 配置 |
| `src/generated/` | 生成物，已提交 |
| `export.map` | 导出的七个符号 |
| `scripts/` | 构建、部署、抓取、生成器 |

## 重新生成 trampoline

```sh
ANDROID_NDK_HOME=/path/to/ndk python3 scripts/generate_layer.py
python3 scripts/generate_layer.py --vk-xml /path/to/vk.xml --print-skipped
python3 scripts/generate_layer.py --allow-signature-mismatch   # 见下
```

`--vk-xml`（或环境变量 `$VK_XML`）指向的是 **Vulkan API Registry**：Khronos 作为
规范一部分维护的、机器可读的 API 权威定义。也就是说，关于「有哪些命令、签名是
什么」，它才是权威，本仓库没有任何需要手工同步的东西。

可以从哪拿到它：

| 来源 | 说明 |
| --- | --- |
| `/usr/share/vulkan/registry/vk.xml` | 默认值指向的位置；由发行版的 `vulkan-headers` 包安装（写这份代码的机器上是 `1:1.4.357.0-1`，下文所说的 "1.4.357" 就来自这里） |
| `git clone https://github.com/KhronosGroup/Vulkan-Docs`，取其中的 `xml/vk.xml` | 上游，永远是最新的 |
| LunarG 的 Vulkan SDK，位于 `share/vulkan/registry/` 下 | 如果你本来就装了 SDK |
| `<ndk>/sources/third_party/vulkan/src/registry/vk.xml` | 只有 NDK 26.x 才有，而且那是 1.3.237 的 registry；NDK 27 已经不再提供。太旧，不值得用 |

registry 不需要与 NDK 头文件版本一致，实际上也不会一致：它通常新很多，而这正是
下面那个过滤存在的意义。不一致不会出问题；正是因为 registry 更新，头文件一支持某
条新命令，它马上就能被捡起来。

只有当一个命令的 `PFN_vk<name>` 存在于 NDK 的 Vulkan 头文件里时，生成器才会为它
产出代码，因为转发函数必须能赋值给那个类型。registry 通常比 NDK 头文件新很多
（这里是 1.4.357 对 1.3.275），这个过滤就是为此存在的：`--print-skipped` 会列出被
跳过的命令，换一个新 NDK 也就是重跑一遍脚本的事。

在写出任何东西之前会跑两项检查，因为签名写错的转发函数会通过 `reinterpret_cast`
编译通过，然后在运行时错误地解释参数：

- 每一个产出的签名都会与头文件里的 `PFN_vk<name>` typedef 逐个参数比对，不一致就
  停止生成；
- 产出的转发函数所提到的每一个类型，都必须由头文件声明过。

## 已验证的内容

在本机的验证（就是写这份代码的那台机器）：

- 用 NDK r28c 交叉编译 `arm64-v8a` 与 `armeabi-v7a`，在 `-Wall -Wextra` 下无警告。
- 库恰好导出 loader 会查找的那七个符号，缺任何一个 `scripts/build.sh` 都会让构建
  失败。（这不是理论问题：如果不对这些入口点显式声明默认可见性，
  `-fvisibility=hidden` 会让链接器把它们丢掉，随后 `--gc-sections` 会删掉每一个
  转发函数。）
- 生成 615 条命令，并且确实存在于二进制里，外加十一个手写入口点。
- 生成器是确定性的，输出按字节序排序且无重复（转发函数的查找是在它上面做二分），
  它跑的三项检查——头文件签名一致、类型已声明、loader 必需的指令——全部通过。

### 对照 loader 源码

layer 与 loader 之间的契约是逐条对照 AOSP 的
`frameworks/native/vulkan/libvulkan`（`api.cpp`、`api_gen.cpp`、`driver.cpp`、
`driver.h`、`layers_extensions.cpp`）核对过的。代码注释里提到的就是这些检查，而
每一条都是硬性要求，不是细节：

| 在 loader 里核对到的 | layer 对应的做法 |
| --- | --- |
| `hwvulkan_dispatch_t` 是一个 **union**，loader 的数据在它的第一个字；`SetData()` 对 instance、physical device、device、queue、command buffer 都会调用 | `KeyOf()` 读那个字作为表 key，这也是 queue 与 command buffer 无需拦截的原因 |
| `SetData()` 在 `driver::CreateInstance`/`CreateDevice` 内部执行，也就是在最后一层 layer 之下 | key 在向下调用返回之后才读取，所以它必然已被填好 |
| `LayerChain::SetupLayerLinks()` 把 `VkLayerInstanceLink`/`VkLayerDeviceLink` 串成链表，最后一层的 `pNext` 为空、其 next-proc 指向 `driver::Get*ProcAddr` | 每次向下调用之前都先把 link 推进到 `pNext` |
| `LayerChain::ModifyCreateInfo()` 在 `pNext` 里放了*两个* `VkLayer*CreateInfo`：一个函数为 `VK_LAYER_FUNCTION_LINK`，另一个携带数据回调 | 遍历链时按 `function` 匹配，而不是只看 `sType` |
| `InitDispatchTable()` 会存 `INIT_PROC(true, instance, GetInstanceProcAddr)` 与 `INIT_PROC(true, dev, GetDeviceProcAddr)`，任一为 null 就让创建失败 | 两个 `vkGet*ProcAddr` 都返回自己，而不是向下转发 |
| `InitDispatchTable()` 把 12 条 instance 级和 121 条 device 级命令标为必需 | `scripts/generate_layer.py` 会检查每一条是否落在 loader 所询问的那张表里，否则停止 |
| 只有当两个枚举函数返回的属性 `memcmp()` 为 0 时，`layers_extensions.cpp` 才会置 `layer.is_global = true` | 两者都返回同一个常量的副本，这也正是 `vkCmd*`/`vkQueueSubmit` 能进链的原因 |
| layer 发现阶段会用空句柄调用各枚举函数，并期待一个成功且为空的回答 | 四个函数都在不解引用任何句柄的前提下返回 `VK_SUCCESS` |

设备验证之后仍然待定的问题：

- 属性访问控制：应用进程究竟能否读取 `debug.vk.systrace.*`，[降低噪声](#降低噪声)
  里的过滤依赖于此。默认配置不需要它。见[已知限制](#已知限制)。

### 在真实设备上

在 OPD2401（Android 16，`ro.build.version.sdk=36`，`ro.build.type=user`，
arm64-v8a，无 root）上，用 settings 路径加一个 debuggable 应用实测：

- loader 枚举到这个 layer，并报告它是 global：

  ```
  D vulkan: searching for layers in '/data/user/0/<package>'
  D vulkan: added global layer 'VK_LAYER_SYSTRACE_apitrace' from library
            '/data/user/0/<package>/libVkLayer_systrace.so'
  I vulkan: Loaded layer VK_LAYER_SYSTRACE_apitrace
  ```

  它走过的三个路径依次是应用的数据目录、它的 `nativeLibraryDir`，以及
  `<base.apk>!/lib/arm64-v8a`。

- layer 建好了自己的表并跟踪了全部命令：

  ```
  I vk.systrace: loaded mask=0x1f include='' exclude='' -> tracing 615 of 615 commands
  I vk.systrace: layer VK_LAYER_SYSTRACE_apitrace enabled for instance 0x... (key 0x...)
  ```

- 对被采样的那次抓取，Perfetto 里出现了 64 个不同 Vulkan 命令的 slice，包括
  `vkQueueSubmit`、`vkQueuePresentKHR`、`vkAcquireNextImageKHR`、
  `vkBeginCommandBuffer`/`vkEndCommandBuffer`、`vkCmdBeginRenderPass`/
  `vkCmdEndRenderPass`、`vkCmdDrawIndexed`、`vkCmdBindPipeline` 和
  `vkCmdPushConstants`，与平台自身的 `dequeueBuffer`/`queueBuffer` slice 并列。
  device 级命令能够出现，正说明 layer 进了 device 链，也就是 `is_global` 生效了。

- 属性路径（`--prop`）在这台设备上**不可用**：`/data/local/debug` 不存在，shell
  用户也无权创建它。`scripts/deploy.sh` 现在会报出这一点，而不是留下一个设好的
  属性却找不到库。想彻底免掉复制到数据目录这一步，见
  [把 so 集成进 APK](#把-so-集成进-apk)。

## 把 so 集成进 APK

把库拷进应用的数据目录是 `deploy.sh` 的默认做法，它需要的只是一个 debuggable 的
构建。想完全免掉这次拷贝，可以让 layer 直接随 APK 发布，因为 loader 会把应用的
APK 当作归档来搜索：`LoadedApk` 把 `<apk>!/lib/<abi>` 加进库搜索路径，而 loader
的 `ForEachFileInPath` 正好理解 `zip!/dir` 这种语法。上面的日志显示的正是这件事。

把 `libVkLayer_systrace.so` 放到应用对应 ABI 的 native 库目录：

```
app/src/main/jniLibs/arm64-v8a/libVkLayer_systrace.so
```

并确保打包时不压缩它：

```groovy
android {
    packaging {
        jniLibs {
            // 等同于 manifest 里的 android:extractNativeLibs="false"
            useLegacyPackaging = false
        }
    }
}
```

这不是可有可无的装饰。loader 只有在下面条件成立时才会枚举归档里的条目：

```cpp
if (entry.method != kCompressStored || entry.offset % kPageSize != 0)
    continue;   // layers_extensions.cpp 里的 ForEachFileInZip
```

也就是说这个库必须是**未压缩存储（STORED）且按页对齐**的；压缩过或没对齐的条目
会被静默跳过，layer 从此就不出现。`useLegacyPackaging = false` 正是让 AGP 以这种
方式存放并对齐 native 库的开关（本文实测设备的页大小是 4096，16 KiB 页的设备上是
16384；构建里已经为后者加了 `-Wl,-z,max-page-size=16384`）。

启用方式不变：仍然由 settings 指定，`adb` 不需要 root 就能写。

```sh
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app <package>
adb shell settings put global gpu_debug_layers VK_LAYER_SYSTRACE_apitrace
```

用 `adb logcat -d | grep 'searching for layers'` 确认：库应当被报告为来自
`.../base.apk!/lib/<abi>`，而不是数据目录。如果没有出现 `added global layer`
那一行，就是条目被压缩了或没有对齐。

## 已知限制

- **过滤用的属性可能读不到。** `debug.vk.systrace.*` 是用
  `__system_property_get()` 读的，而 Android 的属性访问控制可能拒绝应用进程的这次
  读取。如果被拒绝，layer 看到的就相当于属性未设置，于是全量跟踪——这本来也是默认
  行为——而启动日志那一行会暴露这一点。真需要的话，解决办法是改为读取应用数据目录
  下的配置文件，settings 路径本来就有那个目录的写权限。
- `vkCreateInstance` 的 `pNext` 为空时，也就是不在 loader 链里时，会直接以
  `VK_ERROR_INITIALIZATION_FAILED` 拒绝，而不是静默透传。脱离 loader 去跟踪没有
  任何意义。
- registry 里没有、或者有但 NDK 头文件里没有的厂商扩展命令会被转发但不打点（它们
  经由下一层的 `vkGet*ProcAddr` 解析）；驱动没有实现的命令会返回一个提前的兜底值
  并打一条一次性错误日志，而不是崩溃。
- 每次调用一个 slice，不记录参数。参数的取值、队列标签以及 GPU 侧的工作都不在范围
  内；它展示的是应用调用了*哪些*接口，以及每一次花了多长时间。
