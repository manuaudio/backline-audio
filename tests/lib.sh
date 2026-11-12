# Shared helpers for the Backline proofs. Sourced, not run.
# Everything here is throwaway: nodes are owned by a pw-cli client held open
# by this script (object.linger = false), so they vanish when it exits, even
# if the script is killed. Nothing links to the user's devices.
# shellcheck shell=bash

BL_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BL_BIN=${BL_BIN:-$BL_ROOT/build}
BL_PIDS=()

bl_node_id() { # node.name -> id
  pw-dump 2>/dev/null | jq -r --arg n "$1" \
    '.[] | select(.type=="PipeWire:Interface:Node" and .info.props["node.name"]==$n) | .id' | head -1
}

bl_wait_node() { # node.name [tries]
  local i
  for ((i = 0; i < ${2:-100}; i++)); do
    [[ -n $(bl_node_id "$1") ]] && return 0
    sleep 0.05
  done
  echo "timed out waiting for node $1" >&2
  return 1
}

bl_default_source() {
  pw-metadata -n default 0 default.audio.source 2>/dev/null | sed -n "s/.*value:'\([^']*\)'.*/\1/p"
}

bl_default_sink() {
  pw-metadata -n default 0 default.audio.sink 2>/dev/null | sed -n "s/.*value:'\([^']*\)'.*/\1/p"
}

# Snapshot of every node that is not ours: id, name, state. Used to show that
# nothing else in the graph changed.
bl_foreign_nodes() {
  pw-dump 2>/dev/null | jq -r '.[] | select(.type=="PipeWire:Interface:Node")
    | select((.info.props["node.name"] // "") | test("^backline") | not)
    | "\(.id) \(.info.props["node.name"])"' | sort
}

# Compare two snapshots. Nodes of the clap-pipewire project's own test
# harnesses (clap-*), which may run on the same machine at the same time,
# come and go on their own schedule; they are listed but not judged.
bl_same_nodes() { # before after
  local a b ign
  a=$(grep -v ' clap-' "$1" || true); b=$(grep -v ' clap-' "$2" || true)
  ign=$(cat "$1" "$2" | grep ' clap-' | awk '{print $2}' | sort -u | tr '\n' ' ')
  if [[ $a == "$b" ]]; then echo -n "yes ($(wc -l <<<"$a") nodes)"; else echo -n "NO"; diff <(echo "$a") <(echo "$b"); fi
  [[ -n $ign ]] && echo " [ignored other harness: $ign]" || echo
}

# testbed NAME[@DRIVER_PRIORITY]... : one null sink per name, all owned by one
# pw-cli process. The highest priority.driver in a linked group drives it.
bl_testbed() {
  local cmds="" n arg prio
  for arg in "$@"; do
    n=${arg%@*}; prio=1; [[ $arg == *@* ]] && prio=${arg#*@}
    cmds+="create-node adapter { factory.name = support.null-audio-sink node.name = $n node.description = \"Backline test $n\" media.class = Audio/Sink audio.position = [ FL FR ] audio.rate = 48000 object.linger = false priority.session = 1 priority.driver = $prio monitor.channel-volumes = false node.virtual = true }"$'\n'
  done
  # pw-cli reads the commands, then stays connected until killed.
  { printf '%s' "$cmds"; bl_hold; } | pw-cli >/dev/null 2>&1 &
  BL_PIDS+=($!)
  for arg in "$@"; do bl_wait_node "${arg%@*}" || return 1; done
}

# Keeps a pw-cli fed (so its nodes stay) until this script exits, however it
# exits: a plain `sleep` here outlived killed scripts as an orphan.
bl_hold() { while kill -0 $$ 2>/dev/null; do sleep 0.5; done; }

bl_spawn() { # cmd... (tracked for cleanup)
  "$@" &
  BL_PIDS+=($!)
}

bl_cleanup() {
  local p
  for p in "${BL_PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
  # the sleep feeding pw-cli
  pkill -f '^sleep 100000$' -P $$ 2>/dev/null || true
  for p in "${BL_PIDS[@]}"; do wait "$p" 2>/dev/null || true; done
  BL_PIDS=()
}

# Watch the default sink and source for the whole run (WirePlumber may make a
# new test node the default while it exists). Lines in FILE if they move.
bl_watch_defaults() { # sink source FILE
  # A read can fail while pw-metadata is busy (it then prints nothing), so a
  # mismatch counts only if it is still there on a second read.
  ( while sleep 0.5; do
      [[ $(bl_default_sink) == "$1" && $(bl_default_source) == "$2" ]] && continue
      sleep 0.2
      [[ $(bl_default_sink) == "$1" && $(bl_default_source) == "$2" ]] ||
        echo "$(date +%T) sink=$(bl_default_sink) source=$(bl_default_source)" >>"$3"
    done ) &
  BL_WATCH=$!
}
