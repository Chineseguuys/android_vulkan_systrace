#!/usr/bin/env bash
#
# Turns the systrace layer on or off for one debuggable application.
#
# There are two ways to make the Android Vulkan loader pick a layer up, and both
# are implemented here because they fail for different reasons:
#
#   --settings   The loader asks the platform (GraphicsEnv / LoadedApk) for the
#                layer search path of this application, which is the
#                libraryPermittedPath of a debuggable package, i.e. its own data
#                directory /data/user/0/<package>.  The library is copied there
#                through run-as, and three global settings say which application
#                and which layer to enable.  Survives a reboot, and needs no
#                special property access.
#
#   --prop       The loader also looks in /data/local/debug/vulkan, which is
#                writable by the shell, and is told which layer to load by the
#                debug.vulkan.layers property.  Nothing has to be copied into
#                the application's data directory, but the property does not
#                survive a reboot and, on some builds, is only honoured for
#                debuggable applications.
#
# Neither path takes effect for an application that is already running: the
# layer list is read while the process creates its Vulkan instance, so the
# application has to be restarted (this script does that with am force-stop).

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
layer_name=VK_LAYER_SYSTRACE_apitrace
so_name=libVkLayer_systrace.so
abi=
mode=settings
restart=1

usage() {
    cat <<'EOF'
usage: scripts/deploy.sh <on|off|status> <package> [options]

  on <package>     install the layer and enable it for <package>
  off <package>    disable it again and remove the installed library
  status           print the current settings, property and installed copies

options:
  --abi <abi>      ABI of the library to install, default: the ABI the device
                   reports as ro.product.cpu.abi (arm64-v8a or armeabi-v7a)
  --settings       use the global settings and the application data directory
                   (default)
  --prop           use the debug.vulkan.layers property and
                   /data/local/debug/vulkan
  --no-restart      do not force-stop the application afterwards

examples:
  scripts/deploy.sh on com.example.game
  scripts/deploy.sh on com.example.game --prop
  scripts/deploy.sh on com.example.game --abi armeabi-v7a
  scripts/deploy.sh off com.example.game
  scripts/deploy.sh status

After deploying, capture a trace with:

  scripts/capture.sh com.example.game 10
EOF
}

die() {
    echo "deploy.sh: $*" >&2
    exit 1
}

device_shell() {
    adb shell "$@"
}

# The ABI the device itself runs, which is what the library has to be built for.
detect_abi() {
    device_shell getprop ro.product.cpu.abi | tr -d '\r'
}

# The settings path needs the package to be debuggable, because run-as is what
# makes its data directory writable.
check_debuggable() {
    local package=$1
    if ! device_shell run-as "$package" true >/dev/null 2>&1; then
        cat >&2 <<EOF
deploy.sh: run-as $package failed, so the library cannot be copied into the
           application's data directory.  That directory is the layer search
           path the loader uses for the settings path, and run-as only works
           for a package whose manifest sets android:debuggable="true" (or a
           build signed with a debug key).  Use --prop instead, or make the
           application debuggable.
EOF
        exit 1
    fi
}

library_path() {
    local candidate="$root/build/$abi/$so_name"
    [ -f "$candidate" ] || die "no library at $candidate; run scripts/build.sh --abi $abi first"
    echo "$candidate"
}

# --- the two install paths -------------------------------------------------

install_settings() {
    local package=$1 so
    so=$(library_path)

    check_debuggable "$package"

    # /data/local/tmp is traversable by other users but not readable, and adb
    # push leaves the file world readable, so run-as can copy it out of there.
    device_shell rm -f "/data/local/tmp/$so_name"
    adb push "$so" "/data/local/tmp/$so_name" >/dev/null
    device_shell chmod 644 "/data/local/tmp/$so_name"

    # run-as starts in the application's data directory, which is the directory
    # the loader searches for this package.
    device_shell run-as "$package" cp "/data/local/tmp/$so_name" "./$so_name"
    device_shell rm -f "/data/local/tmp/$so_name"

    # GraphicsEnv reads these three: whether to use the list at all, for which
    # application, and the list itself.
    device_shell settings put global enable_gpu_debug_layers 1
    device_shell settings put global gpu_debug_app "$package"
    device_shell settings put global gpu_debug_layers "$layer_name"

    echo "installed $so"
    echo "  -> /data/user/0/$package/$so_name"
    echo "  -> settings global gpu_debug_layers=$layer_name gpu_debug_app=$package"
}

remove_settings() {
    local package=$1
    device_shell settings delete global gpu_debug_layers >/dev/null 2>&1 || true
    device_shell settings delete global gpu_debug_app >/dev/null 2>&1 || true
    device_shell settings delete global enable_gpu_debug_layers >/dev/null 2>&1 || true

    if device_shell run-as "$package" true >/dev/null 2>&1; then
        device_shell run-as "$package" rm -f "./$so_name" >/dev/null 2>&1 || true
    fi
    echo "removed the settings and /data/user/0/$package/$so_name"
}

install_prop() {
    local so
    so=$(library_path)

    # On recent Android /data/local/debug does not exist and the shell may not
    # create it, which makes this path unusable without root.  Say so instead of
    # leaving the property set and the library missing, which looks like the
    # layer was enabled and simply did nothing.
    if ! device_shell mkdir -p /data/local/debug/vulkan 2>/dev/null ||
        ! device_shell test -w /data/local/debug/vulkan; then
        cat >&2 <<EOF
deploy.sh: /data/local/debug/vulkan is not writable from the shell, so the
           property path cannot be used on this device.  That directory has
           been created by init on older releases; on this one it is missing and
           /data/local/debug cannot be created by the shell user without root.
           Use the default settings path instead:

               scripts/deploy.sh on <package>

           which copies the library through run-as into the application's own
           data directory, or build the library into the APK.
EOF
        exit 1
    fi

    if ! adb push "$so" "/data/local/debug/vulkan/$so_name"; then
        die "could not copy $so to /data/local/debug/vulkan"
    fi
    device_shell chmod 644 "/data/local/debug/vulkan/$so_name"

    # The loader reads this property while it enumerates layers.  Setting the
    # property from the shell is allowed; the application reads it as
    # debug.vulkan.layers.
    device_shell setprop debug.vulkan.layers "$layer_name"

    echo "installed $so"
    echo "  -> /data/local/debug/vulkan/$so_name"
    echo "  -> property debug.vulkan.layers=$layer_name"
}

remove_prop() {
    device_shell setprop debug.vulkan.layers '""' 2>/dev/null || true
    device_shell rm -f "/data/local/debug/vulkan/$so_name"
    echo "cleared the property and removed /data/local/debug/vulkan/$so_name"
}

show_status() {
    echo "layer:        $layer_name"
    echo "device abi:   $(detect_abi)"
    echo
    echo "settings:"
    for key in enable_gpu_debug_layers gpu_debug_app gpu_debug_layers; do
        printf '  %-26s %s\n' "$key" \
            "$(device_shell settings get global "$key" | tr -d '\r')"
    done
    echo
    echo "property:"
    printf '  %-26s %s\n' "debug.vulkan.layers" \
        "$(device_shell getprop debug.vulkan.layers | tr -d '\r')"
    echo
    echo "installed copies:"
    for directory in /data/local/debug/vulkan /data/local/tmp; do
        printf '  %s: %s\n' "$directory" \
            "$(device_shell ls -l "$directory/$so_name" 2>/dev/null | tr -d '\r' || true)"
    done
}

# --- argument handling -----------------------------------------------------

[ $# -ge 1 ] || { usage >&2; exit 2; }
action=$1
shift

package=
case "$action" in
    on|off)
        [ $# -ge 1 ] || die "$action needs a package name"
        package=$1
        shift
        ;;
    status|help|--help|-h)
        ;;
    *)
        echo "deploy.sh: unknown action: $action" >&2
        usage >&2
        exit 2
        ;;
esac
while [ $# -gt 0 ]; do
    case "$1" in
        --abi)
            [ $# -ge 2 ] || die "--abi needs a value"
            abi=$2
            shift
            ;;
        --settings) mode=settings ;;
        --prop) mode=prop ;;
        --no-restart) restart=0 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

command -v adb >/dev/null || die "adb not found"

# --help and -h must work without a device, so they are answered before any
# adb call.
case "$action" in
    help|--help|-h)
        usage
        exit 0
        ;;
esac

if ! device_shell true >/dev/null 2>&1; then
    die "no device attached (adb reports none)"
fi

if [ -z "$abi" ]; then
    # The || true matters: without it, a failing adb inside the substitution
    # aborts the whole script under set -e, and the reason is lost.
    abi=$(detect_abi || true)
fi
[ -n "$abi" ] || die "could not read ro.product.cpu.abi; pass --abi"

echo "device:  $(device_shell getprop ro.product.model | tr -d '\r') (Android $(device_shell getprop ro.build.version.release | tr -d '\r'), $abi)"

case "$action" in
    on)
        [ -n "$package" ] || die "on needs a package name"
        if [ "$mode" = settings ]; then
            install_settings "$package"
        else
            install_prop
        fi
        if [ "$restart" = 1 ]; then
            device_shell am force-stop "$package"
            echo
            echo "stopped $package; start it again so it creates a new Vulkan instance"
        fi
        cat <<EOF

Check that the layer was picked up:

  adb logcat -s vk.systrace
EOF
        ;;
    off)
        if [ "$mode" = settings ]; then
            remove_settings "$package"
        else
            remove_prop
        fi
        if [ "$restart" = 1 ]; then
            device_shell am force-stop "$package"
            echo "stopped $package"
        fi
        ;;
    status)
        show_status
        ;;
    help|--help|-h)
        usage
        ;;
esac
