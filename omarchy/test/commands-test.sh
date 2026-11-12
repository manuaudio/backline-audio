#!/bin/bash

# The omarchy-backline-* commands against a stubbed world: busctl, pw-dump,
# backline and the desktop helpers are scratch stubs that log what they were
# asked and answer from fixtures, and the real commands run in a fake HOME.

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
stubs=$scratch/stubs
mkdir -p "$stubs" "$scratch/home"
export HOME=$scratch/home XDG_STATE_HOME=$scratch/home/.local/state XDG_CONFIG_HOME=$scratch/home/.config
export PATH="$stubs:$ROOT/bin:$PATH" LOG=$scratch/log
unset BACKLINE_BIN BACKLINE_MIDI_ALL_MATCH BACKLINE_MIDI_FILTER BACKLINE_INSTRUMENT_TARGET BACKLINE_NODE_FILTER

# A graph with a USB keyboard, Midi Through and a running instrument's MIDI port.
cat >"$scratch/pw-dump.json" <<'JSON'
[
  {"id": 30, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "Midi-Bridge", "media.class": "Midi/Bridge"}}},
  {"id": 31, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 30, "port.name": "Midi Through: Port-0 (capture)", "port.alias": "Midi Through:Midi Through Port-0", "format.dsp": "8 bit raw midi"}}},
  {"id": 32, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 30, "port.name": "KeyStep 37: KeyStep 37 MIDI 1 (capture)", "port.alias": "KeyStep 37:KeyStep 37 MIDI 1", "format.dsp": "8 bit raw midi"}}},
  {"id": 40, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "backline.surge", "media.class": "Stream/Output/Audio"}}},
  {"id": 41, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 40, "port.name": "midi_out", "format.dsp": "8 bit raw midi"}}},
  {"id": 50, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "alsa_input.usb-mic", "media.class": "Audio/Source"}}}
]
JSON
printf '#!/bin/bash\ncat "%s/pw-dump.json"\n' "$scratch" >"$stubs/pw-dump"

# busctl: NameHasOwner answers from $RUNNING; calls are logged and answered
# from small canned replies.
cat >"$stubs/busctl" <<'SH'
#!/bin/bash
args="$*"
if [[ $args == *NameHasOwner* ]]; then
  name=${args##* }
  [[ " ${RUNNING:-} " == *" ${name##*.} "* ]] && echo '{"type":"b","data":[true]}' || echo '{"type":"b","data":[false]}'
  exit 0
fi
echo "busctl ${args#--user --json=short --timeout=25 call }" >>"$LOG"
case $args in
  *"Host1 List"*) echo '{"type":"aa{sv}","data":[[{"id":{"type":"s","data":"surge-xt"}}]]}' ;;
  *"Properties GetAll"*Recorder1*) echo "{\"type\":\"a{sv}\",\"data\":[{\"State\":{\"type\":\"s\",\"data\":\"${REC_STATE:-idle}\"}}]}" ;;
  *"Recorder1 Takes"*) echo '{"type":"s","data":["{\"takes\":[{\"take\":\"take-1\",\"seconds\":192.4}]}"]}' ;;
  *"Recorder1 Stop"*) echo '{"type":"s","data":["/takes/take-1.json"]}' ;;
  *) echo '{"type":"s","data":["ok"]}' ;;
esac
SH
cat >"$stubs/backline" <<'SH'
#!/bin/bash
cat <<'JSON'
{"plugins": [
  {"format": "clap", "id": "org.surge", "name": "Surge XT", "vendor": "Surge", "path": "/clap/Surge XT.clap", "grade": "pass", "kinds": ["instrument"]},
  {"format": "clap", "id": "cardinal.synth", "name": "Cardinal Synth", "vendor": "DISTRHO", "path": "/clap/Cardinal.clap", "grade": "fail", "reasons": ["check crashed (SIGABRT)"], "kinds": ["instrument"]},
  {"format": "clap", "id": "dexed", "name": "Dexed", "vendor": "Digital Suburban", "path": "/clap/Dexed.clap", "grade": "warn", "kinds": ["instrument"]}
]}
JSON
SH
printf '#!/bin/bash\necho "notify $*" >>"$LOG"\n' >"$stubs/omarchy-notification-send"
printf '#!/bin/bash\necho "shell $*" >>"$LOG"\n' >"$stubs/omarchy-shell"
printf '#!/bin/bash\nshift\nprintf "%%s\\n" "$@" >"%s/offered"\n[[ -n ${ANSWER:-} ]] && printf "%%s\\n" "$ANSWER" || exit 1\n' "$scratch" >"$stubs/omarchy-menu-select"
chmod +x "$stubs"/*

# ---------------------------------------------------------------- metadata
for command in "$ROOT"/bin/omarchy-backline-*; do
  [[ $(head -n1 "$command") == "#!/bin/bash" ]] || fail "$(basename "$command") starts with #!/bin/bash"
  head -n 80 "$command" | grep -q '^# omarchy:summary=' || fail "$(basename "$command") has an omarchy:summary"
done
pass "every command has #!/bin/bash and omarchy:summary metadata"

# ---------------------------------------------------------------- MIDI
inputs=$(bash -c "source '$ROOT/lib/omarchy-backline.sh'; bl_midi_inputs")
[[ $(jq -r '.[0].all' <<<"$inputs") == "true" ]] || fail "the first MIDI choice is every keyboard" "$inputs"
[[ $(jq -r '[.[].label] | join("|")' <<<"$inputs") == "|KeyStep 37 MIDI 1" ]] ||
  fail "Midi Through and Backline's own ports are not offered; a port named after its device reads as the port" "$inputs"
match=$(jq -r '.[1].match' <<<"$inputs")
node -e '
const [match, all] = process.argv.slice(1)
const hay = "Midi-Bridge:KeyStep 37: KeyStep 37 MIDI 1 (capture) | KeyStep 37:KeyStep 37 MIDI 1"
if (!new RegExp(match, "i").test(hay)) throw new Error("own match fails")
if (new RegExp(match, "i").test("Midi-Bridge:Other: x (capture) | Other")) throw new Error("matches another port")
const re = new RegExp(all, "i")
if (!re.test(hay) || re.test("Midi-Bridge:Midi Through: Port-0 (capture) | Midi Through:Midi Through Port-0"))
  throw new Error("the every-keyboard match is wrong")
' "$match" "$(jq -r '.[0].match' <<<"$inputs")" || fail "the matches select their port as the worker's ECMAScript regex does"
pass "MIDI inputs: every keyboard first, Midi Through left out, each match escaped for the worker"

# ---------------------------------------------------------------- add
: >"$LOG"
ANSWER=$'Dexed\tDigital Suburban · WARN' RUNNING="Host" omarchy-backline-instrument-add >/dev/null
offered=$(cat "$scratch/offered")
[[ $(sed -n 1p <<<"$offered") == *"Surge XT"* && $(sed -n 3p <<<"$offered") == *"Cardinal Synth"* ]] ||
  fail "the picker lists pass before warn before fail" "$offered"
grep -q 'Host1 Load sa{sv} /clap/Dexed.clap 3 id s dexed plugin-id s dexed midi-follow s ^(?!Midi-Bridge:Midi Through)' "$LOG" ||
  fail "the chosen instrument loads by path, with its id and every keyboard" "$(cat "$LOG")"
[[ $(jq -r .plugin "$XDG_STATE_HOME/backline/omarchy/last-instrument.json") == "/clap/Dexed.clap" ]] || fail "the last instrument is remembered"
ANSWER="" RUNNING="Host" omarchy-backline-instrument-add >/dev/null || true
[[ $(sed -n 1p "$scratch/offered") == *"Dexed"* ]] || fail "the last instrument is offered first next time" "$(cat "$scratch/offered")"
pass "add: the picker orders by last use and grade, and loads by path following every keyboard"

: >"$LOG"
RUNNING="Host" omarchy-backline-instrument-add "Surge XT" >/dev/null
grep -q 'Load sa{sv} /clap/Surge XT.clap 3 id s surge-xt-2 ' "$LOG" || fail "an id in use gets a number" "$(cat "$LOG")"
pass "add: a second instance of the same instrument gets the next id"

: >"$LOG"
if RUNNING="Host" omarchy-backline-instrument-add "Cardinal Synth" >/dev/null 2>&1; then
  fail "a failing instrument is refused without --force"
fi
grep -q 'notify .*Cardinal Synth fails validation check crashed (SIGABRT)' "$LOG" || fail "the refusal says why" "$(cat "$LOG")"
grep -q 'Load' "$LOG" && fail "nothing is loaded when refused"
RUNNING="Host" omarchy-backline-instrument-add "Cardinal Synth" --force >/dev/null
grep -q 'Load sa{sv} /clap/Cardinal.clap 4 .* force b true' "$LOG" || fail "--force asks the host to force" "$(cat "$LOG")"
pass "add: a plugin graded fail is refused and says why, unless forced"

# ---------------------------------------------------------------- record
: >"$LOG"
RUNNING="Recorder" omarchy-backline-record source alsa_input.usb-mic on >/dev/null
RUNNING="Recorder" REC_STATE=armed omarchy-backline-record source backline.surge on >/dev/null
[[ $(cat "$XDG_STATE_HOME/backline/omarchy/record-sources.json") == '["alsa_input.usb-mic","backline.surge"]' ]] ||
  fail "arming adds to the remembered set" "$(cat "$XDG_STATE_HOME/backline/omarchy/record-sources.json")"
grep -q 'Recorder1 Arm asa{sv} 1 alsa_input.usb-mic 1 pre-roll d 5' "$LOG" || fail "arming the first source arms the recorder with pre-roll" "$(cat "$LOG")"
grep -A1 'Recorder1 Stop' "$LOG" | grep -q 'Recorder1 Arm asa{sv} 2 alsa_input.usb-mic backline.surge 1 pre-roll d 5' ||
  fail "a change to an armed set re-arms with the new set" "$(cat "$LOG")"
if RUNNING="Recorder" REC_STATE=recording omarchy-backline-record source alsa_input.usb-mic off >/dev/null 2>&1; then
  fail "the set can't change during a take"
fi
pass "record: arming remembers the set and re-arms the recorder; a take in progress keeps its tracks"

: >"$LOG"
RUNNING="Recorder" REC_STATE=recording omarchy-backline-record toggle >/dev/null
grep -q 'notify -g 󰻃 Saved take-1 (3:12)' "$LOG" || fail "stopping says which take and how long" "$(cat "$LOG")"
grep -q 'Recorder1 Arm' "$LOG" || fail "after a take the set stands by again, so the next has pre-roll"
pass "record: the hotkey stops a take, names it, and arms again"

# ---------------------------------------------------------------- i18n
mkdir -p "$XDG_CONFIG_HOME/omarchy"
catalogs=$scratch/catalogs
cp -r "$ROOT/i18n" "$catalogs"
printf '{"record.saved": "Gravado %%1 (%%2)"}\n' >"$catalogs/pt.json"
printf '{"record.started": "Gravando %%1 faixas"}\n' >"$catalogs/pt-BR.json"
echo pt-BR >"$XDG_CONFIG_HOME/omarchy/language"
lookup() { bash -c "source '$ROOT/lib/omarchy-backline.sh'; BL_ROOT='$scratch/catalogs-root'; bl_t \"\$@\"" _ "$@"; }
mkdir -p "$scratch/catalogs-root" && ln -s "$catalogs" "$scratch/catalogs-root/i18n"
[[ $(lookup record.started 3) == "Gravando 3 faixas" ]] || fail "the regional catalog wins" "$(lookup record.started 3)"
[[ $(lookup record.saved take-1 3:12) == "Gravado take-1 (3:12)" ]] || fail "then the base language" "$(lookup record.saved take-1 3:12)"
[[ $(lookup record.busy) == "Stop the take first" ]] || fail "then English"
[[ $(lookup no.such.key) == "no.such.key" ]] || fail "then the key"
pass "notifications follow the Omarchy language: pt-BR, then pt, then English, with placeholders"
