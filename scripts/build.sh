#!/usr/bin/env bash
#
# Builds libVkLayer_systrace.so for one or more Android ABIs.
#
# The generated sources under src/generated/ are produced by
# scripts/generate_layer.py first, so that the trampolines always match the
# Vulkan headers of the NDK that is used for the build.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
platform=android-24
build_type=Release
generate=1
abis=(arm64-v8a)

usage() {
    cat <<'EOF'
usage: scripts/build.sh [options]

  --abi <list>        comma separated ABIs, default arm64-v8a
                      (supported: arm64-v8a, armeabi-v7a, x86_64, x86)
  --all-abis          build arm64-v8a and armeabi-v7a
  --ndk <path>        Android NDK directory, default $ANDROID_NDK_HOME,
                      $ANDROID_NDK_ROOT, $ANDROID_HOME/ndk/<newest> or
                      ~/Android/Sdk/ndk/<newest>
  --platform <api>    Android API level to build for, default android-24
  --debug             build with CMAKE_BUILD_TYPE=Debug
  --no-generate       skip scripts/generate_layer.py and use src/generated/
                      as it is
  -h, --help          show this text
EOF
}

die() {
    echo "build.sh: $*" >&2
    exit 1
}

# Picks the newest directory inside $1 whose name looks like an NDK version.
newest_child() {
    local base=$1
    [ -d "$base" ] || return 1
    # shellcheck disable=SC2012
    ls -1 "$base" 2>/dev/null | sort -V | tail -n 1
}

detect_ndk() {
    local candidate version

    if [ -n "$ndk" ]; then
        [ -d "$ndk" ] || die "NDK directory not found: $ndk"
        return
    fi

    for candidate in "$HOME/Android/Sdk/ndk" "$HOME/Library/Android/sdk/ndk" \
                     "${ANDROID_HOME:-}/ndk" "${ANDROID_SDK_ROOT:-}/ndk"; do
        [ -d "$candidate" ] || continue
        version=$(newest_child "$candidate") || continue
        ndk="$candidate/$version"
        break
    done

    [ -n "$ndk" ] || die "no NDK found; pass --ndk <path> or set ANDROID_NDK_HOME"
    [ -d "$ndk" ] || die "NDK directory not found: $ndk"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --abi)
            [ $# -ge 2 ] || die "--abi needs a value"
            IFS=',' read -r -a abis <<<"$2"
            shift
            ;;
        --all-abis)
            abis=(arm64-v8a armeabi-v7a)
            ;;
        --ndk)
            [ $# -ge 2 ] || die "--ndk needs a value"
            ndk=$2
            shift
            ;;
        --platform)
            [ $# -ge 2 ] || die "--platform needs a value"
            platform=$2
            shift
            ;;
        --debug)
            build_type=Debug
            ;;
        --no-generate)
            generate=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "build.sh: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

detect_ndk

# The generator picks the headers up through this variable as well.
export ANDROID_NDK_HOME=$ndk

cmake_bin=$(command -v cmake) || die "cmake not found"
ninja_bin=$(command -v ninja) || die "ninja not found"
python_bin=$(command -v python3) || die "python3 not found"

toolchain="$ndk/build/cmake/android.toolchain.cmake"
[ -f "$toolchain" ] || die "not an NDK directory: $ndk"

echo "NDK:      $ndk"
echo "cmake:    $cmake_bin ($("$cmake_bin" --version | head -n 1))"
echo "ninja:    $ninja_bin"
echo "platform: $platform"
echo "ABIs:     ${abis[*]}"

if [ "$generate" = 1 ]; then
    echo
    echo "== generating trampolines =="
    "$python_bin" "$root/scripts/generate_layer.py"
fi

for abi in "${abis[@]}"; do
    build_dir="$root/build/$abi"
    echo
    echo "== $abi =="
    "$cmake_bin" \
        -S "$root" \
        -B "$build_dir" \
        -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="$platform" \
        -DANDROID_STL=c++_static \
        -DANDROID_ARM_NEON=ON

    "$cmake_bin" --build "$build_dir" --target VkLayer_systrace
done

# CMakeLists.txt asks for compile_commands.json (clangd and editors are what
# read it).  CMake writes it inside the build tree, and the tools look for it in
# the source tree, so link the one of the first ABI into place.  A relative
# target keeps the link valid if the checkout is moved, -n replaces an existing
# one, and a regular file is removed first so an earlier copy is not silently
# kept instead.
compile_commands="$root/compile_commands.json"
if [ -f "$compile_commands" ] && [ ! -L "$compile_commands" ]; then
    rm -f "$compile_commands"
fi
if [ -f "$root/build/${abis[0]}/compile_commands.json" ]; then
    ln -sfn "build/${abis[0]}/compile_commands.json" "$compile_commands"
fi

echo
echo "== result =="

# The entry points that the loader looks up, which are exactly the ones
# export.map keeps and src/layer_entry.cpp marks with VKST_EXPORT.
expected_exports="vkGetInstanceProcAddr
vkGetDeviceProcAddr
vkEnumerateInstanceLayerProperties
vkEnumerateInstanceExtensionProperties
vkEnumerateDeviceLayerProperties
vkEnumerateDeviceExtensionProperties
vkNegotiateLoaderLayerInterfaceVersion"
expected_count=7

llvm_nm="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
for abi in "${abis[@]}"; do
    so="$root/build/$abi/libVkLayer_systrace.so"
    [ -f "$so" ] || die "build did not produce $so"
    printf '%-14s %s (%s)\n' "$abi" "$so" "$(du -h "$so" | cut -f1)"
    if [ -x "$llvm_nm" ]; then
        exports=$("$llvm_nm" --dynamic --defined-only "$so" |
                  awk '$2 ~ /^[TBWDR]$/ { print $3 }' | sort)
        count=$(printf '%s\n' "$exports" | grep -c . || true)
        echo "               exports ($count):"
        printf '%s\n' "$exports" | sed 's/^/                 /'

        # A library that is missing these still links and still looks like a
        # layer, but the loader either rejects it or loads it and traces
        # nothing, so the check is fatal rather than a warning.  (Getting here
        # with zero exports is what -fvisibility=hidden without an explicit
        # default visibility on the entry points looks like.)
        missing=$(printf '%s\n' \
            "$expected_exports" | sort |
            comm -23 - <(printf '%s\n' "$exports") | tr -d '\n')
        if [ -n "$missing" ]; then
            die "$abi does not export the entry points the loader needs;" \
                "missing: $missing (see export.map and VKST_EXPORT in" \
                "src/layer_entry.cpp)"
        fi
        if [ "$count" -ne "$expected_count" ]; then
            echo "               warning: expected only the $expected_count" \
                 "entry points of export.map"
        fi
    fi
done

cat <<EOF

Deploy with:

  scripts/deploy.sh on <package> --abi ${abis[0]}
  scripts/capture.sh <package> 10

EOF
