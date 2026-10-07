#!/usr/bin/env bash
#
# Records a Perfetto trace of one application and pulls it to the host.
#
# The reason this needs a script of its own is the atrace_apps field.  ATrace_
# beginSection in the NDK is fixed to ATRACE_TAG_APP, and that tag is enabled
# only for the processes named in the debug.atrace.app_<n> properties - it does
# not depend on atrace_categories at all.  Perfetto's linux.ftrace data source
# is what writes those properties, so a config without atrace_apps produces a
# trace with every other kind of data and not a single Vulkan slice.
#
# If perfetto is not available or refuses the config, the script falls back to
# the atrace command line tool, whose text output also opens in ui.perfetto.dev.

set -euo pipefail

seconds=10
out=
buffer_kb=65536
categories=gfx

usage() {
    cat <<'EOF'
usage: scripts/capture.sh <process> [seconds] [options]

  <process>        command line of the process to trace, which for an Android
                   application is its package name, e.g. com.example.game
  [seconds]        how long to record, default 10

options:
  --out <file>     where to write the trace, default trace-<process>-<time>.pftrace
  --buffer <kb>    size of the trace buffer, default 65536
  --categories <c> atrace categories, default gfx

examples:
  scripts/capture.sh com.example.game
  scripts/capture.sh com.example.game 20 --out spin.pftrace

Open the result at https://ui.perfetto.dev (or with `perfetto` locally) and look
at the thread track of the application: one slice per Vulkan call, named after
the command, e.g. vkQueueSubmit.
EOF
}

die() {
    echo "capture.sh: $*" >&2
    exit 1
}

[ $# -ge 1 ] || { usage >&2; exit 2; }

case "$1" in
    -h|--help) usage; exit 0 ;;
esac

process=$1
shift

if [ $# -ge 1 ] && [ "${1#-}" = "$1" ]; then
    seconds=$1
    shift
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --out) [ $# -ge 2 ] || die "--out needs a value"; out=$2; shift ;;
        --buffer) [ $# -ge 2 ] || die "--buffer needs a value"; buffer_kb=$2; shift ;;
        --categories) [ $# -ge 2 ] || die "--categories needs a value"; categories=$2; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

command -v adb >/dev/null || die "adb not found"

if ! adb shell true >/dev/null 2>&1; then
    die "no device attached (adb reports none)"
fi

[ -n "$out" ] || out="trace-$process-$(date +%Y%m%d-%H%M%S).pftrace"

duration_ms=$((seconds * 1000))
device_trace=/data/misc/perfetto-traces/$(basename "$out" .pftrace).pftrace

echo "process:  $process"
echo "duration: ${seconds}s"
echo "output:   $out"

# --- perfetto ---------------------------------------------------------------

config=$(mktemp)
trap 'rm -f "$config"' EXIT

cat >"$config" <<EOF
buffers {
  size_kb: $buffer_kb
  fill_policy: RING_BUFFER
}

data_sources {
  config {
    name: "linux.ftrace"
    target_buffer: 0
    ftrace_config {
      # Required: without this the ATRACE_TAG_APP sections this layer emits are
      # not enabled in the traced process, and the trace contains no slices.
      atrace_apps: "$process"
      atrace_categories: "$categories"
      buffer_size_kb: $buffer_kb
      drain_period_ms: 250
    }
  }
}

duration_ms: $duration_ms
EOF

record_with_perfetto() {
    if ! adb shell "command -v perfetto" >/dev/null 2>&1; then
        echo "perfetto is not on the device" >&2
        return 1
    fi

    adb shell rm -f "$device_trace" >/dev/null 2>&1 || true

    if ! adb shell perfetto -c - --txt -o "$device_trace" <"$config"; then
        echo "perfetto refused the configuration" >&2
        return 1
    fi

    if ! adb shell "ls -l $device_trace" >/dev/null 2>&1; then
        echo "perfetto produced no trace file" >&2
        return 1
    fi

    adb pull "$device_trace" "$out" >/dev/null
    adb shell rm -f "$device_trace" >/dev/null 2>&1 || true
    return 0
}

# --- atrace fallback --------------------------------------------------------

record_with_atrace() {
    echo "falling back to atrace; the result is a text trace" >&2
    out=${out%.pftrace}.atrace.txt
    # --app is what turns on the APP tag for this process, the same thing
    # atrace_apps does for perfetto.
    adb shell atrace --app="$process" -c "$categories" -t "$seconds" >"$out"
    if [ ! -s "$out" ]; then
        die "atrace produced an empty trace; is $process running?"
    fi
    return 0
}

if record_with_perfetto; then
    echo
    echo "wrote $out"
else
    record_with_atrace
    echo
    echo "wrote $out (atrace text trace, also opens in ui.perfetto.dev)"
fi

cat <<EOF

If the trace has no Vulkan slices, check in this order:

  1. Is the layer loaded?   adb logcat -s vk.systrace  should show
     "layer VK_LAYER_SYSTRACE_apitrace enabled for instance ...".
  2. Was the process running for the whole recording?  "$process" has to match
     the process command line exactly.
  3. Are commands enabled?  The same log line reports how many of the commands
     the current debug.vk.systrace.* properties leave switched on.
EOF
