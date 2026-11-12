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
