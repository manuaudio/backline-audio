#!/usr/bin/env bash
# Backline's real plugin host (backline-plugin-worker, one process per plugin)
# on a private PipeWire daemon at quantum 64 / 48 kHz, no realtime priority.
# CHAINS parallel chains of LEN workers each; each chain is fed by a sine test
# source; the unit cycles EQ8 -> Comp -> CHOWTape -> Dragonfly Hall.
# Prints one JSON line: driver xruns (ERR) over SECONDS and the driver's load.
#   pw-capacity.sh BUILD_DIR CHAINS LEN SECONDS
# SPDX-License-Identifier: MIT
set -u
BUILD=${1:?build dir}; CHAINS=${2:?}; LEN=${3:?}; SECS=${4:-20}
HERE=$(cd "$(dirname "$0")" && pwd)
export PIPEWIRE_REMOTE=pipewire-bltest
L=$HOME/.clap/lsp-plugins.clap
P=("$L" "$L" "$HOME/.clap/CHOWTapeModel.clap" "$HOME/.clap/DragonflyHallReverb.clap")
I=(in.lsp-plug.para_equalizer_x8_stereo in.lsp-plug.compressor_stereo org.chowdsp.CHOWTapeModel michaelwillis.dragonfly.hall)
pids=()
cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
  for p in "${pids[@]}"; do wait "$p" 2>/dev/null; done
  pkill -f 'backline-plugin-worker --no-dbus -A -p .* -n bltest\.' 2>/dev/null
  pkill -f 'backline-test-source --sines 110:2 bltest\.' 2>/dev/null
  [ -n "${PWPID:-}" ] && kill "$PWPID" 2>/dev/null && wait "$PWPID" 2>/dev/null
}
trap cleanup EXIT INT TERM
[ -S "$XDG_RUNTIME_DIR/pipewire-bltest" ] && { echo '{"error":"pipewire-bltest already running"}'; exit 1; }
pipewire -c "$HERE/pw-bltest.conf" >/dev/null 2>&1 & PWPID=$!
for _ in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/pipewire-bltest" ] && break; sleep 0.1; done
for c in $(seq 1 "$CHAINS"); do
  "$BUILD/backline-test-source" --sines 110:2 "bltest.src$c" >/dev/null 2>&1 & pids+=($!)
done
sleep 1
for c in $(seq 1 "$CHAINS"); do
  prev="bltest.src$c"
  for k in $(seq 1 "$LEN"); do
    u=$(( (k - 1) % 4 )); n="bltest.c$c.k$k"
    "$BUILD/backline-plugin-worker" --no-dbus -A -p "${P[$u]}" -i "${I[$u]}" -n "$n" -S "$prev" >/dev/null 2>&1 & pids+=($!)
    prev=$n
  done
done
# wait until every worker node exists and the graph runs
want=$(( CHAINS * LEN ))
for _ in $(seq 120); do
  have=$(pw-cli ls Node 2>/dev/null | grep -c 'node.name = "bltest.c')
  [ "$have" -ge "$want" ] && break; sleep 0.5
done
# the test source's ports carry no FL/FR position, so the head of each chain is linked here
for c in $(seq 1 "$CHAINS"); do
  pw-link "bltest.src$c:out_00" "bltest.c$c.k1:in_FL" 2>/dev/null
  pw-link "bltest.src$c:out_01" "bltest.c$c.k1:in_FR" 2>/dev/null
done
sleep 1
links=$(pw-cli ls Link 2>/dev/null | grep -c 'PipeWire:Interface:Link')
sleep 3
rss_mb=$(ps -o rss= -p "$(IFS=,; echo "${pids[*]}")" 2>/dev/null | awk '{s+=$1} END {print int(s/1024)}')
# per-cycle timings from PipeWire's profiler, judged against the hardware deadline (pwprof.py)
prof=$(cd "${TMPDIR:-/tmp}" && timeout $((SECS + 10)) pw-profiler -r pipewire-bltest -J -n $((SECS * 750)) 2>/dev/null | python3 "$HERE/pwprof.py")
PROF=$prof python3 -c "
import json, os
d={'chains':$CHAINS,'len':$LEN,'workers':$want,'nodes_up':int('$have'),'links':int('$links'),
   'rss_mb_total':int('$rss_mb' or 0),'load1':open('/proc/loadavg').read().split()[0]}
d.update(json.loads(os.environ.get('PROF') or '{}'))
print(json.dumps(d))"
