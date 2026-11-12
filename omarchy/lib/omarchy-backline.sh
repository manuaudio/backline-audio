# Shared by the omarchy-backline-* commands. Sourced, not run.
#
# Everything here talks to Backline's session-bus services through busctl, so
# the commands need nothing beyond systemd and jq, both Omarchy defaults. Reads
# never start a service that isn't running: a closed recorder or clock is
# reported as such instead of being woken by a panel opening. Actions do start
# them (the recorder, clock and registry are D-Bus activated; the host is
# started here, since it has no activation file).
# shellcheck shell=bash

BL_BUS=io.github.manuaudio.Backline
BL_PATH=/io/github/manuaudio/Backline
BL_STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/backline/omarchy"

# bl_call SERVICE METHOD [SIGNATURE ARGS...]: the reply as busctl JSON
# ({"type": ..., "data": [...]}) on stdout; a D-Bus error on stderr, exit 1.
bl_call() {
  local service=$1 method=$2
  shift 2
  busctl --user --json=short --timeout=25 call "$BL_BUS.$service" "$BL_PATH/$service" "$BL_BUS.${service}1" "$method" "$@"
}

# bl_running SERVICE: the service owns its bus name now (never activates it).
bl_running() {
  [[ $(busctl --user --json=short call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus NameHasOwner s "$BL_BUS.$1" 2>/dev/null) == *true* ]]
}

# bl_props SERVICE: every property as one flat JSON object, or {} when the
# service is not running.
bl_props() {
  if bl_running "$1"; then
    busctl --user --json=short call "$BL_BUS.$1" "$BL_PATH/$1" org.freedesktop.DBus.Properties GetAll s "$BL_BUS.${1}1" 2>/dev/null |
      jq -c '.data[0] | with_entries(.value = .value.data)'
  else
    echo '{}'
  fi
}

# The busctl reply's error, without busctl's "Call failed: " prefix, for a
# notification a person reads.
bl_error_text() {
  sed -e 's/^Call failed: //' -e 's/^Failed to [^:]*: //' <<<"$1" | head -n1
}

bl_backline() {
  if [[ -n ${BACKLINE_BIN:-} ]]; then
    "$BACKLINE_BIN/backline" "$@"
  else
    backline "$@"
  fi
}

# The host has no activation file: start it as a transient user service the
# first time an instrument is loaded, so it is supervised and logs to the
# journal. Tests set BACKLINE_HOSTD_CMD to run their own.
bl_ensure_host() {
  bl_running Host && return 0
  if [[ -n ${BACKLINE_HOSTD_CMD:-} ]]; then
    eval "$BACKLINE_HOSTD_CMD" >/dev/null 2>&1 &
    disown
  else
    systemd-run --user --quiet --collect --unit=backline-hostd \
      --description="Backline plugin host" "${BACKLINE_BIN:+$BACKLINE_BIN/}backline-hostd" >/dev/null 2>&1
  fi
  local _
  for _ in {1..100}; do
    bl_running Host && return 0
    sleep 0.05
  done
  return 1
}

# MIDI sources as JSON rows {label, match}: first "every keyboard", then each
# source. `match` is the regex the plugin worker's --midi-follow tests against
# "<node.name>:<port.name> | <port.alias>". Both list MIDI controllers only,
# the rule the worker itself applies (common/bl_midi.h): a port on a MIDI
# device node (media.class Midi/...), never Midi Through, never one of
# Backline's own nodes (the clock's MTC output), never an app's MIDI output.
bl_midi_inputs() {
  pw-dump 2>/dev/null | jq -c '
    def esc: gsub("(?<c>[.^$|()\\[\\]{}*+?\\\\])"; "\\\(.c)");
    [.[] | select(.type == "PipeWire:Interface:Node") | {key: (.id | tostring), value: .info.props}] as $nodes
    | ($nodes | from_entries) as $props
    | [.[] | select(.type == "PipeWire:Interface:Port")
        | select(.info.direction == "output")
        | select((.info.props["format.dsp"] // "") | test("midi|UMP"))
        | ($props[(.info.props["node.id"] | tostring)] // {}) as $n
        | {node: ($n["node.name"] // ""), class: ($n["media.class"] // ""), port: .info.props["port.name"], alias: (.info.props["port.alias"] // .info.props["port.name"])}
        | select(.class | startswith("Midi/"))
        | select(.node != "" and (.node | startswith("backline") | not))
        | select((.alias | startswith("backline.") or startswith("backline:")) | not)
        | select((.alias + " " + .port) | test("Midi Through") | not)
        | select($filter == "" or (.alias | test($filter)))
        | (.alias | split(":")) as $a
        | {label: (if ($a | length) < 2 then .alias elif ($a[1:] | join(":") | startswith($a[0])) then ($a[1:] | join(":")) else $a[0] + " · " + ($a[1:] | join(":")) end),
           match: ("^" + (.node | esc) + ":" + (.port | esc) + " \\|")}]
    | [{label: "", match: $all, all: true}] + .' --arg all "${BACKLINE_MIDI_ALL_MATCH:-^(?!Midi-Bridge:Midi Through)}" --arg filter "${BACKLINE_MIDI_FILTER:-}"
}

BL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

# bl_t KEY [ARGS...]: one notification or prompt string in the language picked
# under Setup > Language, from Backline's own catalogs (omarchy/i18n/<code>.json),
# falling back to the base language, then English, then the key, the way
# omarchy-i18n does for Omarchy's catalog. %1, %2, ... are filled from ARGS.
bl_t() {
  local key=$1 language base text i
  shift
  language=$(head -n1 "${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/language" 2>/dev/null)
  [[ $language =~ ^[a-z]{2,3}(-[A-Za-z]{2})?$ ]] || language=en
  base=${language%%-*}
  text=$(jq -r --arg k "$key" 'map(.[$k] // empty | if type == "object" then .other else . end) | map(select(. != "")) | .[0] // $k' \
    <(cat "$BL_ROOT/i18n/$language.json" "$BL_ROOT/i18n/$base.json" "$BL_ROOT/i18n/en.json" 2>/dev/null | jq -s '.') 2>/dev/null)
  [[ -n $text ]] || text=$key
  for ((i = $#; i >= 1; i--)); do
    text=${text//%$i/${!i}}
  done
  printf '%s' "$text"
}

bl_notify() {
  omarchy-notification-send "$@" >/dev/null 2>&1 || true
}

# bl_push TARGET...: tell the shell's Backline panels to read their state again.
bl_push() {
  local target
  for target in "$@"; do
    omarchy-shell -q "$target" refresh >/dev/null 2>&1 || true
  done
}

bl_state_file() {
  mkdir -p "$BL_STATE_DIR"
  echo "$BL_STATE_DIR/$1.json"
}

# bl_remember KEY JSON / bl_recall KEY [DEFAULT]: small per-user choices (the
# armed set, the last instrument, the chase input), one JSON file each,
# written atomically.
bl_remember() {
  local file
  file=$(bl_state_file "$1")
  printf '%s\n' "$2" >"$file.tmp" && mv -f "$file.tmp" "$file"
}

bl_recall() {
  local file
  file=$(bl_state_file "$1")
  if [[ -s $file ]]; then
    cat "$file"
  else
    echo "${2:-null}"
  fi
}
