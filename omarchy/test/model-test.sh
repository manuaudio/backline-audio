#!/bin/bash

# The panels' shared model (plugin/Model.js), in Node: the logic that
# decides what the panels offer and say.

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

run_node_test <<'JS' && pass "instrument picker: last used first, then by grade, failing last"
const choices = [
  { name: "Odin2", grade: "pass", path: "/o" },
  { name: "Cardinal Synth", grade: "fail", path: "/cs" },
  { name: "Dexed", grade: "warn", path: "/d" },
  { name: "Surge XT", grade: "pass", path: "/s" },
]
assert.deepStrictEqual(Model.sortChoices(choices, "/s").map(c => c.name), ["Surge XT", "Odin2", "Dexed", "Cardinal Synth"])
assert.deepStrictEqual(Model.sortChoices(choices, "").map(c => c.name), ["Odin2", "Surge XT", "Dexed", "Cardinal Synth"])
JS

run_node_test <<'JS' && pass "filter: every word must match name or vendor"
const rows = [{ name: "Surge XT", vendor: "Surge Synth Team" }, { name: "Dexed", vendor: "Digital Suburban" }]
assert.deepStrictEqual(Model.filterChoices(rows, "sur xt", "").map(r => r.name), ["Surge XT"])
assert.deepStrictEqual(Model.filterChoices(rows, "suburban", "").map(r => r.name), ["Dexed"])
assert.strictEqual(Model.filterChoices(rows, "  ", "").length, 2)
assert.strictEqual(Model.filterChoices(rows, "sur dex", "").length, 0)
JS

run_node_test <<'JS' && pass "MIDI: an instrument's follow match maps to its pill; an unplugged one to none"
const inputs = [{ match: "^(?!x)", all: true, label: "" }, { match: "^A \\|", label: "KeyStep" }]
assert.strictEqual(Model.midiIndex(inputs, "^A \\|"), 1)
assert.strictEqual(Model.midiIndex(inputs, "^gone"), -1)
assert.strictEqual(Model.midiLabel(inputs[0]), "All keyboards")
assert.strictEqual(Model.midiLabel(undefined), "Unplugged")
JS

run_node_test <<'JS' && pass "plugins: pills filter by kind and by issues; the finding drops stderr detail"
const products = [
  { name: "Surge XT", kinds: ["instrument"], worst: "pass", formats: [{ format: "clap", grade: "pass", checked: "run" }] },
  { name: "Cardinal Synth", kinds: ["instrument"], worst: "fail", formats: [
    { format: "clap", grade: "fail", checked: "run", reasons: ["check crashed (SIGABRT, signal 6); last stderr: what(): Settings file"] },
    { format: "lv2", grade: "pass", checked: "run" }] },
  { name: "Chow Tape", kinds: ["audio-effect"], worst: "pass", formats: [{ format: "vst3", grade: "pass", checked: "load" }] },
]
assert.deepStrictEqual(Model.filterProducts(products, "", "issues").map(p => p.name), ["Cardinal Synth"])
assert.deepStrictEqual(Model.filterProducts(products, "", "effect").map(p => p.name), ["Chow Tape"])
assert.strictEqual(Model.findingLine(products[1]), "CLAP: check crashed (SIGABRT, signal 6)")
assert.strictEqual(Model.findingLine(products[2]), "Loads cleanly (load check only)")
assert.strictEqual(Model.findingLine(products[0]), "Loads, runs and restores its state")
JS

run_node_test <<'JS' && pass "record sources: inputs, instruments, outputs; Backline's own nodes left out"
const nodes = [
  { id: 1, name: "alsa_output.usb", description: "Speakers", mediaClass: "Audio/Sink" },
  { id: 2, name: "backline.surge-xt", description: "Surge XT", mediaClass: "Stream/Output/Audio" },
  { id: 3, name: "alsa_input.usb", description: "Mic", mediaClass: "Audio/Source" },
  { id: 4, name: "backline.soundcheck", description: "x", mediaClass: "Stream/Output/Audio" },
  { id: 5, name: "firefox", description: "Firefox", mediaClass: "Stream/Output/Audio" },
]
assert.deepStrictEqual(Model.recordSources(nodes).map(s => s.kind + ":" + s.name),
  ["input:alsa_input.usb", "instrument:backline.surge-xt", "output:alsa_output.usb"])
assert.deepStrictEqual(Model.recordSources(nodes, "^backline").map(s => s.name), ["backline.surge-xt"])
JS

run_node_test <<'JS' && pass "timecode: non-drop and 29.97 drop-frame"
assert.strictEqual(Model.timecode(48000 * 3600, 48000, 25), "01:00:00:00")
assert.strictEqual(Model.timecode(48000 * 1.5, 48000, 24), "00:00:01:12")
// 29.97 DF: frame 1800 (one minute of 30 nominal fps less two dropped) reads 00:01:00;02.
assert.strictEqual(Model.timecode(1800 / 29.97 * 48000, 48000, 29.97), "00:01:00;02")
// Every tenth minute keeps its frames: 17982 frames is 00:10:00;00.
assert.strictEqual(Model.timecode(17982 / 29.97 * 48000, 48000, 29.97), "00:10:00;00")
assert.strictEqual(Model.timecode(0, 0, 25), "--:--:--:--")
JS

run_node_test <<'JS' && pass "transport: projected frame while rolling, frozen while stopped"
assert.strictEqual(Model.projectedFrame({ rolling: true, frame: 1000, rate: 48000, atMs: 0 }, 500), 1000 + 24000)
assert.strictEqual(Model.projectedFrame({ rolling: false, frame: 1000, rate: 48000, atMs: 0 }, 500), 1000)
JS

run_node_test <<'JS' && pass "chase status: locked ok, freewheel warn, no signal muted"
assert.deepStrictEqual(Model.chaseStatus({ LtcMode: "chase", Locked: true, ChaseError: -2 }), { text: "LOCKED · -2 FR", tone: "ok" })
assert.strictEqual(Model.chaseStatus({ LtcMode: "chase", Locked: false, Rolling: true }).tone, "warn")
assert.strictEqual(Model.chaseStatus({ MtcMode: "chase" }).text, "NO SIGNAL")
assert.strictEqual(Model.chaseStatus({ LinkEnabled: true, LinkPeers: 2 }).text, "2 LINK PEERS")
JS

run_node_test <<'JS' && pass "record hero: armed tracks, recording time, translated through tr"
assert.strictEqual(Model.recordHero({ state: "armed", armed: ["a", "b"] }, 0), "ARMED · 2 TRACKS")
assert.strictEqual(Model.recordHero({ state: "armed", armed: ["a"] }, 0), "ARMED · 1 TRACK")
assert.strictEqual(Model.recordHero({ state: "recording", armed: [] }, 3725), "RECORDING 01:02:05")
assert.strictEqual(Model.recordHero({ state: "recording" }, 5, (k, e) => k === "record.heroRecording" ? "OPNAME %1" : e), "OPNAME 00:00:05")
JS

run_node_test <<'JS' && pass "pro audio: the status from the command, or none; the header, labels and where the cursor parks"
// The status comes from a command, so anything else it prints (nothing while
// PipeWire is down, an error) must hide the section rather than draw a zero buffer.
const pro = Model.parseProStatus('{"enabled":true,"buffer":256,"rate":48000,"realtime":false}')
assert.deepStrictEqual(pro, { enabled: true, buffer: 256, rate: 48000, realtime: false })
for (const text of ['', 'PipeWire is not running', '[]', '{"enabled":true,"buffer":0,"rate":48000}'])
  assert.strictEqual(Model.parseProStatus(text), null, text)
assert.strictEqual(Model.proTitle({ enabled: false, buffer: 1024, rate: 48000 }), 'PRO AUDIO')
assert.strictEqual(Model.proTitle(pro), 'PRO AUDIO · 5.3 ms')
assert.strictEqual(Model.rateLabel(44100), '44.1')
assert.strictEqual(Model.rateLabel(48000), '48')
assert.strictEqual(Model.bufferTooltip(128, 48000), '128 frames, 2.7 ms')
assert.strictEqual(Model.pillIndex(Model.PRO_BUFFERS, 256), 2)
assert.strictEqual(Model.pillIndex(Model.PRO_BUFFERS, 2048), 0)
JS

run_node_test <<'JS' && pass "audio setup: combined devices and MIDI ports from the commands; the ports that send, devices first"
assert.deepStrictEqual(Model.parseCombined('[{"name":"Studio","active":true,"mirror":false},{"active":true}]'), [{ name: 'Studio', active: true, mirror: false }])
assert.deepStrictEqual(Model.parseCombined('PipeWire lists no audio devices.'), [])
assert.strictEqual(Model.combinedCaption({ name: 'Everywhere', active: false, mirror: true }), 'Multi-output · stopped')
const ports = Model.parseMidiPorts(JSON.stringify([
  { address: '14:0', client: 'Midi Through', port: 'Midi Through Port-0', kind: 'virtual', protocol: 'MIDI 1.0', direction: 'both', sendsTo: [] },
  { address: '130:0', client: 'Bitwig Studio', port: 'output', kind: 'app', protocol: 'MIDI 1.0', direction: 'source', sendsTo: [] },
  { address: '128:0', client: 'Surge XT', port: 'input', kind: 'app', protocol: 'MIDI 1.0', direction: 'destination', sendsTo: [] },
  { address: '24:0', client: 'Keystation', port: 'Keystation MIDI 1', kind: 'hardware', protocol: 'MIDI 2.0', direction: 'both', sendsTo: ['128:0'] },
  { port: 'no address' }
]))
assert.deepStrictEqual(Model.midiSources(ports).map(p => p.address), ['24:0', '130:0'])
assert.strictEqual(Model.portLabel(ports[3]), 'Keystation MIDI 1')
assert.strictEqual(Model.portLabel(ports[1]), 'Bitwig Studio output')
assert.strictEqual(Model.portCaption(ports[3], ports), 'MIDI 2.0 · to Surge XT')
assert.strictEqual(Model.portCaption(ports[1], ports), 'MIDI 1.0 · not connected')
assert.deepStrictEqual(Model.parseMidiPorts('The ALSA sequencer is not loaded'), [])
JS

run_node_test <<'JS' && pass "audio setup: one cursor over the switch, the pills while on, devices, add, then MIDI"
const kinds = rows => rows.map(r => r.kind + (r.index === undefined ? '' : r.index)).join(' ')
assert.strictEqual(kinds(Model.setupRows({ enabled: true }, [{}, {}], [{}])), 'pro buffer rate device0 device1 add midi0')
assert.strictEqual(kinds(Model.setupRows({ enabled: false }, [], [])), 'pro add')
// PipeWire not answering: no pro audio rows, the rest stays.
assert.strictEqual(kinds(Model.setupRows(null, [{}], [])), 'device0 add')
JS
