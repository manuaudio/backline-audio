#!/bin/bash

# The Audio Setup commands (omarchy-backline-pro, -aggregate, -midi) against a
# stubbed world: pw-metadata, pw-dump, systemctl, ps, aconnect and the desktop
# helpers are scratch stubs that log what they were asked and answer from
# fixtures, and the real commands run in a fake HOME. Nothing here talks to a
# real PipeWire or sequencer.

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
stubs=$scratch/stubs
data=$scratch/data
mkdir -p "$stubs" "$scratch/home"
export HOME=$scratch/home XDG_STATE_HOME=$scratch/home/.local/state XDG_CONFIG_HOME=$scratch/home/.config
export XDG_RUNTIME_DIR=$scratch/run TEST_DATA=$data
export PATH="$stubs:$ROOT/bin:$PATH"
state=$XDG_STATE_HOME/backline/omarchy/pro-audio
pro_unit=$XDG_CONFIG_HOME/systemd/user/backline-pro-audio.service

# PipeWire's settings metadata is a key=value file the pw-metadata stub reads
# and writes; ps reports the data loop's scheduling class from another.
cat >"$stubs/pw-metadata" <<'STUB'
#!/bin/bash
[[ -f $TEST_DATA/settings ]] || exit 1
if (( $# == 5 )); then
  sed -i "/^$4=/d" "$TEST_DATA/settings"
  echo "$4=$5" >>"$TEST_DATA/settings"
else
  while IFS='=' read -r key value; do
    echo "update: id:0 key:'$key' value:'$value' type:''"
  done <"$TEST_DATA/settings"
fi
STUB
cat >"$stubs/systemctl" <<'STUB'
#!/bin/bash
echo "$*" >>"$TEST_DATA/systemctl"
case "$*" in
  *"show -p MainPID"*) echo 4242 ;;
  *is-enabled*) grep -q '^--user enable backline-pro' "$TEST_DATA/systemctl" && ! grep -q '^--user disable backline-pro' "$TEST_DATA/systemctl" ;;
esac
exit 0
STUB
printf '#!/bin/bash\ncat "$TEST_DATA/cls"\n' >"$stubs/ps"
printf '#!/bin/bash\necho "$*" >>"$TEST_DATA/notifications"\n' >"$stubs/omarchy-notification-send"
printf '#!/bin/bash\necho "$*" >>"$TEST_DATA/shell"\n' >"$stubs/omarchy-shell"
chmod +x "$stubs"/*

setting() {
  sed -n "s/^$1=//p" "$data/settings"
}

# A freshly started PipeWire: nothing forced, its audio thread at normal priority.
fresh_pipewire() {
  rm -rf "$data" "$XDG_RUNTIME_DIR"
  mkdir -p "$data" "$XDG_RUNTIME_DIR"
  printf '%s\n' clock.rate=48000 clock.quantum=1024 clock.min-quantum=32 clock.max-quantum=2048 \
    clock.force-quantum=0 clock.force-rate=0 >"$data/settings"
  printf '%s\n' TS TS >"$data/cls"
}

# ---------------------------------------------------------------- pro audio
fresh_pipewire
omarchy-backline-pro on >/dev/null 2>&1 || fail "pro audio turns on"
[[ $(setting clock.force-quantum) == 256 && $(setting clock.force-rate) == 48000 ]] ||
  fail "pro audio defaults to 256 frames without realtime and locks the current rate" "$(cat "$data/settings")"
grep -q '^--user enable backline-pro-audio.service' "$data/systemctl" ||
  fail "pro audio enables the unit that sets it again when PipeWire restarts"
[[ -f $state && -f $pro_unit ]] || fail "pro audio saves its choice and installs its unit"
grep -qx "ExecStart=$ROOT/bin/omarchy-backline-pro on" "$pro_unit" && grep -qx "ConditionPathExists=$state" "$pro_unit" ||
  fail "the unit runs this very command, only while the choice exists" "$(cat "$pro_unit")"
grep -qx 'backline.setup refresh' <(sed 's/^-q //' "$data/shell") || fail "pro audio tells the Audio Setup panel" "$(cat "$data/shell")"
omarchy-backline-pro enabled || fail "the menu's guard sees pro audio on"
pass "pro audio defaults to 256 frames without realtime, locks the rate and persists"

fresh_pipewire
rm -f "$state"
printf '%s\n' RR TS >"$data/cls"
omarchy-backline-pro on >/dev/null 2>&1
[[ $(setting clock.force-quantum) == 128 ]] || fail "pro audio defaults to 128 frames once PipeWire runs realtime"
pass "pro audio defaults to 128 frames once PipeWire runs realtime"

# Each choice changes only itself, and is what a restarted PipeWire gets back.
omarchy-backline-pro on --buffer 64 >/dev/null 2>&1
omarchy-backline-pro on --rate 96000 >/dev/null 2>&1
[[ $(setting clock.force-quantum) == 64 && $(setting clock.force-rate) == 96000 ]] ||
  fail "a rate change keeps the chosen buffer" "$(cat "$data/settings")"
fresh_pipewire
omarchy-backline-pro on >/dev/null 2>&1
[[ $(setting clock.force-quantum) == 64 && $(setting clock.force-rate) == 96000 ]] ||
  fail "a restarted PipeWire gets the saved buffer and rate back" "$(cat "$data/settings")"
pass "buffer and rate choices are kept apart and restored after a PipeWire restart"

# Turning off clears only what pro audio forced, never a buffer forced since.
sed -i 's/^clock.force-quantum=.*/clock.force-quantum=512/' "$data/settings"
omarchy-backline-pro off >/dev/null 2>&1 || fail "pro audio turns off"
[[ $(setting clock.force-quantum) == 512 && $(setting clock.force-rate) == 0 ]] ||
  fail "off leaves a buffer another program forced" "$(cat "$data/settings")"
[[ ! -e $state && ! -e $pro_unit ]] || fail "off forgets the choice and removes the unit"
! omarchy-backline-pro enabled || fail "the menu's guard sees pro audio off"
pass "pro audio off clears only what it forced and forgets the choice"

fresh_pipewire
if omarchy-backline-pro on --buffer 100 >/dev/null 2>&1; then
  fail "a buffer that is not a power of two is refused"
fi
[[ $(setting clock.force-quantum) == 0 && ! -e $state ]] || fail "a refused buffer changes nothing"
pass "pro audio refuses a buffer that is not a power of two"

omarchy-backline-pro on --buffer 64 >/dev/null 2>&1
[[ $(omarchy-backline-pro status --json) == '{"enabled":true,"buffer":64,"rate":48000,"realtime":false}' ]] ||
  fail "status --json reports the live buffer and rate" "$(omarchy-backline-pro status --json)"
pass "pro audio status --json reports the live buffer and rate"

# With PipeWire down there is nothing to clear, but off still forgets the choice.
rm -f "$data/settings"
[[ -z $(omarchy-backline-pro status --json 2>/dev/null) ]] || fail "status --json prints nothing while PipeWire is down"
omarchy-backline-pro off >/dev/null 2>&1 || fail "pro audio turns off while PipeWire is down"
[[ ! -e $state && ! -e $pro_unit ]] || fail "off with PipeWire down forgets the choice and removes the unit"
pass "pro audio turns off while PipeWire is down"

# ---------------------------------------------------------------- aggregate
# The graph: two USB inputs (one without channel positions), a speaker and
# headphones. A combined device shows up once its unit is enabled, unless the
# scenario says it never does.
cat >"$stubs/pw-dump" <<'STUB'
#!/bin/bash
node() { printf '{"type":"PipeWire:Interface:Node","info":{"props":%s}},' "$1"; }
printf '['
node '{"node.name":"alsa_input.usb-a","media.class":"Audio/Source","node.description":"Interface A","audio.channels":2,"audio.position":"[ FL, FR ]"}'
node '{"node.name":"alsa_input.usb-b","media.class":"Audio/Source","node.description":"Interface B","audio.channels":4}'
node '{"node.name":"alsa_output.speakers","media.class":"Audio/Sink","node.description":"Speakers","audio.channels":2,"audio.position":"[ FL, FR ]"}'
node '{"node.name":"bluez_output.headphones","media.class":"Audio/Sink","node.description":"Headphones","audio.channels":2,"audio.position":"[ FL, FR ]"}'
if [[ ! -f $TEST_DATA/never-appears ]]; then
  for unit in $(sed -n 's/^--user enable --now backline-aggregate@\(.*\)\.service$/\1/p' "$TEST_DATA/systemctl" 2>/dev/null); do
    grep -q "disable --now backline-aggregate@$unit.service" "$TEST_DATA/systemctl" && continue
    node "{\"node.name\":\"backline_aggregate_${unit//-/_}\",\"media.class\":\"Audio/Source\",\"audio.channels\":6}"
  done
fi
printf '{}]\n'
STUB
printf '#!/bin/bash\ncat "$TEST_DATA/picks"\n' >"$stubs/omarchy-menu-select"
printf '#!/bin/bash\necho "Field Rig"\n' >"$stubs/omarchy-menu-input"
chmod +x "$stubs"/*

fresh() {
  rm -rf "$data" "$XDG_CONFIG_HOME"
  mkdir -p "$data"
}

fragment() {
  cat "$XDG_CONFIG_HOME/pipewire/backline-aggregate-$1.conf.d/50-combine.conf"
}

# Channels stack in member order, and a member without positions gets AUX ones.
fresh
omarchy-backline-aggregate create "Studio Rig" alsa_input.usb-a alsa_input.usb-b >/dev/null || fail "an aggregate of two inputs is made"
conf=$(fragment studio-rig)
[[ $conf == *"combine.mode = source"* && $conf == *"audio.position = [ AUX0 AUX1 AUX2 AUX3 AUX4 AUX5 ]"* ]] ||
  fail "an aggregate input has every member's channels" "$conf"
[[ $conf == *'node.name = "alsa_input.usb-a"'*"combine.audio.position = [ AUX0 AUX1 ]"*"audio.position = [ FL FR ]"* ]] ||
  fail "the first member takes channels 1 and 2" "$conf"
[[ $conf == *'node.name = "alsa_input.usb-b"'*"combine.audio.position = [ AUX2 AUX3 AUX4 AUX5 ]"*"audio.position = [ AUX0 AUX1 AUX2 AUX3 ]"* ]] ||
  fail "the second member follows on channels 3 to 6" "$conf"
grep -qx -- '--user enable --now backline-aggregate@studio-rig.service' "$data/systemctl" ||
  fail "the device runs in its own unit"
grep -q '^ExecStart=/usr/bin/pipewire -c backline-aggregate-%i.conf$' "$XDG_CONFIG_HOME/systemd/user/backline-aggregate@.service" &&
  grep -q 'libpipewire-module-client-node' "$XDG_CONFIG_HOME/pipewire/backline-aggregate-studio-rig.conf" ||
  fail "the unit hosts the device in its own PipeWire client"
[[ $(omarchy-backline-aggregate list --json) == '[{"name":"Studio Rig","active":false,"mirror":false}]' ]] ||
  fail "list --json names the devices made here" "$(omarchy-backline-aggregate list --json)"
pass "an aggregate stacks its members' channels in order, each in its own PipeWire client"

fresh
omarchy-backline-aggregate create --mirror Everywhere alsa_output.speakers bluez_output.headphones >/dev/null || fail "a multi-output device is made"
conf=$(fragment everywhere)
[[ $conf == *"combine.mode = sink"* && $conf == *"stream.dont-remix = false"* && $(grep -c 'combine.audio.position = \[ FL FR \]' <<<"$conf") == 2 ]] ||
  fail "a multi-output device sends the same stereo pair to every output" "$conf"
[[ $(omarchy-backline-aggregate list --json | jq -r '.[0].mirror') == "true" ]] || fail "list --json says it mirrors"
pass "a multi-output device mirrors stereo onto every output"

fresh
if omarchy-backline-aggregate create Mixed alsa_input.usb-a alsa_output.speakers >/dev/null 2>&1; then
  fail "inputs and outputs are not combined"
fi
if omarchy-backline-aggregate create --mirror Mics alsa_input.usb-a alsa_input.usb-b >/dev/null 2>&1; then
  fail "inputs are not mirrored"
fi
if omarchy-backline-aggregate create 'Rig" }' alsa_input.usb-a alsa_input.usb-b >/dev/null 2>&1; then
  fail "a name that would break PipeWire's config is refused"
fi
[[ ! -e $XDG_CONFIG_HOME/pipewire ]] || fail "a refused device writes nothing"
pass "inputs and outputs are not mixed, only outputs mirror, and names stay plain"

# A device PipeWire never lists is undone rather than left half made.
fresh
touch "$data/never-appears"
if omarchy-backline-aggregate create Ghost alsa_input.usb-a alsa_input.usb-b >/dev/null 2>&1; then
  fail "a device that never appears is reported as failed"
fi
[[ ! -e $XDG_CONFIG_HOME/pipewire/backline-aggregate-ghost.conf && ! -e $XDG_CONFIG_HOME/systemd/user/backline-aggregate@.service ]] ||
  fail "a device that never appears is removed again"
pass "a device that never appears in PipeWire is removed again"

# The menu's flow: members picked by their node names, then a name.
fresh
printf '["Interface A\\t2 ch · alsa_input.usb-a","Interface B\\t4 ch · alsa_input.usb-b"]\n' >"$data/picks"
omarchy-backline-aggregate create >/dev/null || fail "the menu's flow makes a device"
[[ $(fragment field-rig) == *'node.name = "alsa_input.usb-a"'*'node.name = "alsa_input.usb-b"'* ]] ||
  fail "the menu's picks become the members, in order"
grep -q 'Field Rig is ready' "$data/notifications" || fail "the menu's flow says the device is ready" "$(cat "$data/notifications")"
pass "the menu's flow makes the device from the picked members"

omarchy-backline-aggregate remove "Field Rig" >/dev/null || fail "a combined device is removed by name"
[[ ! -e $XDG_CONFIG_HOME/pipewire/backline-aggregate-field-rig.conf && ! -e $XDG_CONFIG_HOME/systemd/user/backline-aggregate@.service ]] ||
  fail "removing the last device removes its files and the unit"
[[ $(omarchy-backline-aggregate list --json) == '[]' ]] || fail "nothing is listed after the last removal"
pass "removing the last combined device removes its files and the unit"

# ---------------------------------------------------------------- MIDI
# A sequencer with a MIDI 2.0 keyboard sending to one synth app and to
# PipeWire's bridge, a second synth, the kernel's Midi Through bus, and the
# plumbing the list leaves out.
fresh
cat >"$data/clients" <<'SEQ'
Client info
  cur  clients : 6
Client   0 : "System" [Kernel Legacy]
  Port   0 : "Timer" (Rwe-) [In/Out]
    Connecting To: 142:0
Client  14 : "Midi Through" [Kernel Legacy]
  Port   0 : "Midi Through Port-0" (RWe-) [In/Out]
Client  24 : "Keystation" [Kernel UMP MIDI2]
  Port   0 : "Keystation MIDI 1" (RWeX) [In/Out]
    Connecting To: 128:0[real:0], 142:0
Client 128 : "Surge XT" [User Legacy]
  Port   0 : "input" (-We-) [Out]
    Connected From: 24:0[real:0]
Client 129 : "Vital" [User Legacy]
  Port   0 : "input" (-We-) [Out]
Client 142 : "PipeWire-System" [User Legacy]
  Port   0 : "input" (rwe-) [In/Out]
SEQ
export BACKLINE_SEQ_CLIENTS_PATH=$data/clients
printf '#!/bin/bash\necho "$*" >>"$TEST_DATA/aconnect"\n' >"$stubs/aconnect"
cat >"$stubs/omarchy-menu-select" <<'STUB'
#!/bin/bash
printf '%s\n' "$@" >"$TEST_DATA/menu"
printf '["Vital\\tinput · 129:0"]\n'
STUB
chmod +x "$stubs"/*

json=$(omarchy-backline-midi list --json)
[[ $(jq -c '.[] | select(.address == "24:0")' <<<"$json") == '{"address":"24:0","client":"Keystation","port":"Keystation MIDI 1","kind":"hardware","protocol":"MIDI 2.0","direction":"both","sendsTo":["128:0"]}' ]] ||
  fail "a MIDI 2.0 keyboard lists as hardware with its connection, not the bridge's" "$json"
[[ $(jq -r '.[] | select(.address == "128:0") | .direction' <<<"$json") == "destination" ]] ||
  fail "a port others can only write to is a destination" "$json"
[[ $(jq -r 'map(.client) | join(",")' <<<"$json") == "Midi Through,Keystation,Surge XT,Vital" ]] ||
  fail "the sequencer's own client and PipeWire's bridge are left out" "$json"
pass "midi lists each port's kind, protocol, direction and connections"

# Picking from the menu makes the picks the source's connections, no more.
: >"$data/aconnect"
omarchy-backline-midi connect 24:0 || fail "the menu's flow connects a source"
grep -qx -- '--selected' "$data/menu" && grep -qx $'Surge XT\tinput · 128:0' "$data/menu" ||
  fail "the menu opens with the source's connections ticked" "$(cat "$data/menu")"
! grep -q 'Keystation MIDI 1 · 24:0' "$data/menu" || fail "a source is not offered as its own destination"
[[ $(cat "$data/aconnect") == $'-d 24:0 128:0\n24:0 129:0' ]] ||
  fail "unticked destinations are disconnected and ticked ones connected, and the bridge is left alone" "$(cat "$data/aconnect")"
grep -q 'backline.setup refresh' "$data/shell" || fail "a MIDI change tells the Audio Setup panel"
pass "midi connect makes the destinations picked in the menu the source's connections"
