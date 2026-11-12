// Pure helpers for the mixer window, shared by its QML and its tests.
.pragma library

// The fader law: +6 dB at the top, 0 dB at 0.85, -20 at 0.5, -60 at the bottom.
var FADER_MAX = 6
var FADER_MIN = -60

function faderPosition(db) {
  if (db >= FADER_MAX) return 1
  if (db <= FADER_MIN) return 0
  if (db >= 0) return 0.85 + 0.15 * db / FADER_MAX
  if (db >= -20) return 0.5 + 0.35 * (db + 20) / 20
  return 0.5 * (db - FADER_MIN) / (-20 - FADER_MIN)
}

function faderDb(position) {
  var p = Math.max(0, Math.min(1, position))
  if (p >= 0.85) return FADER_MAX * (p - 0.85) / 0.15
  if (p >= 0.5) return -20 + 20 * (p - 0.5) / 0.35
  return FADER_MIN + (-20 - FADER_MIN) * p / 0.5
}

// A readout: one decimal, a true minus sign, "-inf" at the floor.
function dbText(db) {
  if (db === undefined || db === null || isNaN(db) || db <= -59.95) return "−∞"
  var s = (Math.round(db * 10) / 10).toFixed(1)
  return db < 0 ? "−" + s.substring(1) : (db > 0 ? "+" + s : s)
}

// The meter law, the same as meterPosition() in meter.cpp (a test holds them
// equal): 0 dBFS at the top, -20 at half, -40 at a quarter, -60 at the bottom.
function meterPosition(db) {
  if (db >= 0) return 1
  if (db >= -20) return 0.5 + (db + 20) / 40
  if (db >= -40) return 0.25 + (db + 40) / 80
  if (db >= -60) return (db + 60) / 80
  return 0
}

var METER_TICKS = [0, -3, -6, -12, -20, -30, -40, -60]

// Strip order and labels: inputs, instruments, outputs.
function kindCaption(kind) {
  if (kind === "input") return "IN"
  if (kind === "instrument") return "INST"
  if (kind === "output") return "OUT"
  return ""
}

// Group boundaries, for the section labels above the strips.
function groupStarts(strips) {
  var starts = []
  for (var i = 0; i < strips.length; i++)
    if (i === 0 || strips[i].kind !== strips[i - 1].kind) starts.push(i)
  return starts
}

// HH:MM:SS:FF of a frame (29.97 is drop-frame), as in the Transport panel.
function timecode(frame, rate, fps) {
  if (!rate || rate <= 0) return "--:--:--:--"
  var drop = Math.abs(fps - 29.97) < 0.01
  var nominal = Math.round(fps)
  var frames = Math.floor((frame / rate) * fps + 1e-6)
  if (drop) {
    var tens = Math.floor(frames / 17982), rem = frames % 17982
    frames += 18 * tens + (rem > 1 ? 2 * Math.floor((rem - 2) / 1798) : 0)
  }
  var ff = frames % nominal
  var total = Math.floor(frames / nominal)
  function two(v) { return v < 10 ? "0" + v : String(v) }
  return two(Math.floor(total / 3600) % 24) + ":" + two(Math.floor(total / 60) % 60) + ":" + two(total % 60) + (drop ? ";" : ":") + two(ff)
}

function elapsed(seconds) {
  var s = Math.max(0, Math.floor(seconds || 0))
  function two(v) { return v < 10 ? "0" + v : String(v) }
  return two(Math.floor(s / 3600)) + ":" + two(Math.floor(s / 60) % 60) + ":" + two(s % 60)
}

function shortLength(seconds) {
  var s = Math.max(0, Math.round(seconds || 0))
  return Math.floor(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + (s % 60)
}

// The MIDI input an instrument follows, in words.
function midiText(match) {
  if (!match) return ""
  if (match.indexOf("(?!") >= 0 || /^\^Midi-Bridge:[^ ]*$/.test(match)) return "ALL MIDI"
  // "^Midi-Bridge:Arturia KeyStep 37:Arturia KeyStep 37 MIDI 1 \(capture\) \|"
  // reads as its device, "Arturia KeyStep 37".
  var m = /^\^[^:]+:([^:\\]+)/.exec(match)
  return m ? m[1].trim() : match
}
