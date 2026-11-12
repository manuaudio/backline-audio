// Pure logic shared by Backline's four panels (Instruments, Record, Plugins,
// Transport). Plain JavaScript with no QML, so Node can load it for tests;
// each panel imports it as "Model.js" (the plugins carry a copy, since a
// third-party plugin directory is installed on its own). Functions that make
// visible text take an optional tr(key, english) and default to the English,
// which the tests assert.

function english(key, text) { return text }

function pick(tr) { return typeof tr === "function" ? tr : english }

function fill(text, args) {
  var out = String(text)
  for (var i = args.length; i >= 1; i--) out = out.split("%" + i).join(String(args[i - 1]))
  return out
}

// ---------------------------------------------------------------- parsing

function parseJson(text, fallback) {
  try {
    var value = JSON.parse(String(text || ""))
    return value === null || value === undefined ? fallback : value
  } catch (e) {
    return fallback
  }
}

// ------------------------------------------------------------- instruments

var GRADE_ORDER = { pass: 0, warn: 1, fail: 2 }

function gradeRank(grade) {
  var rank = GRADE_ORDER[grade]
  return rank === undefined ? 3 : rank
}

// The picker's order: the last instrument used, then by grade, then by name.
// A failing instrument stays listed (it says why) but last.
function sortChoices(choices, last) {
  var list = (choices || []).slice()
  list.sort(function(a, b) {
    var la = (last && (a.path === last || a.name === last)) ? 0 : 1
    var lb = (last && (b.path === last || b.name === last)) ? 0 : 1
    if (la !== lb) return la - lb
    var ga = gradeRank(a.grade), gb = gradeRank(b.grade)
    if (ga !== gb) return ga - gb
    return String(a.name).toLowerCase() < String(b.name).toLowerCase() ? -1 : 1
  })
  return list
}

// Every query word must appear in the name or vendor ("sur xt" finds Surge XT).
function matches(row, query) {
  var words = String(query || "").toLowerCase().split(/\s+/).filter(function(w) { return w.length > 0 })
  if (words.length === 0) return true
  var hay = (String(row.name || "") + " " + String(row.vendor || "")).toLowerCase()
  for (var i = 0; i < words.length; i++) if (hay.indexOf(words[i]) < 0) return false
  return true
}

function filterChoices(choices, query, last) {
  return sortChoices(choices, last).filter(function(row) { return matches(row, query) })
}

// The MIDI pill an instrument's follow match corresponds to: an index into
// inputs, or -1 when it follows something the list no longer shows (a
// keyboard that was unplugged).
function midiIndex(inputs, match) {
  for (var i = 0; i < (inputs || []).length; i++) if (inputs[i].match === match) return i
  return -1
}

function midiLabel(input, tr) {
  var t = pick(tr)
  if (!input) return t("midi.none", "Unplugged")
  return input.all ? t("midi.all", "All keyboards") : String(input.label)
}

function instrumentStatus(row, tr) {
  var t = pick(tr)
  var state = row ? row.state : ""
  if (state === "running") return t("instruments.running", "RUNNING")
  if (state === "starting") return t("instruments.starting", "STARTING")
  if (state === "restarting") return t("instruments.restarting", "RESTARTING")
  if (state === "stopping") return t("instruments.stopping", "STOPPING")
  if (state === "failed") return t("instruments.failed", "FAILED")
  return String(state || "").toUpperCase()
}

// Status semantics, one place for every panel and the mixer: a token, not a
// colour, so each view maps it onto the theme.
function statusTone(state) {
  if (state === "running" || state === "pass" || state === "locked") return "ok"
  if (state === "starting" || state === "restarting" || state === "warn" || state === "freewheel") return "warn"
  if (state === "failed" || state === "fail" || state === "recording") return "urgent"
  return "muted"
}

function instrumentsHero(model, tr) {
  var t = pick(tr)
  if (!model || !model.running) return t("instruments.hostIdle", "NOTHING LOADED")
  var n = (model.instances || []).length
  var failed = (model.instances || []).filter(function(i) { return i.state === "failed" }).length
  if (failed > 0) return fill(t("instruments.heroFailed", "%1 FAILED"), [failed])
  if (n === 0) return t("instruments.hostIdle", "NOTHING LOADED")
  return n === 1 ? t("instruments.heroOne", "1 PLAYING") : fill(t("instruments.heroMany", "%1 PLAYING"), [n])
}

// ----------------------------------------------------------------- plugins

var PLUGIN_PILLS = ["all", "instrument", "effect", "issues"]

function filterProducts(products, query, pill) {
  return (products || []).filter(function(p) {
    if (!matches(p, query)) return false
    if (pill === "instrument") return (p.kinds || []).indexOf("instrument") >= 0
    if (pill === "effect") return (p.kinds || []).indexOf("instrument") < 0
    if (pill === "issues") return p.worst !== "pass"
    return true
  })
}

function formatsLine(product) {
  return (product.formats || []).map(function(f) { return String(f.format).toUpperCase() }).join(" · ")
}

// The finding a person needs, first: the first reason of the worst format.
function findingLine(product, tr) {
  var t = pick(tr)
  var formats = product.formats || []
  var worst = null
  for (var i = 0; i < formats.length; i++)
    if (!worst || gradeRank(formats[i].grade) > gradeRank(worst.grade)) worst = formats[i]
  if (!worst || worst.grade === "pass") {
    var loadOnly = formats.some(function(f) { return f.checked === "load" })
    return loadOnly && formats.every(function(f) { return f.checked === "load" })
      ? t("plugins.passLoad", "Loads cleanly (load check only)")
      : t("plugins.passRun", "Loads, runs and restores its state")
  }
  var reason = (worst.reasons || [])[0] || ""
  // The registry's reason can carry a whole stderr line; the first clause is
  // the finding, the rest is detail for the details view.
  reason = String(reason).replace(/; last stderr:.*$/, "")
  return String(worst.format).toUpperCase() + ": " + reason
}

function gradeLabel(grade, tr) {
  var t = pick(tr)
  if (grade === "pass") return t("grade.pass", "PASS")
  if (grade === "warn") return t("grade.warn", "WARN")
  if (grade === "fail") return t("grade.fail", "FAIL")
  return String(grade || "").toUpperCase()
}

// ------------------------------------------------------------------ record

// Sources the Record panel offers, from primitive node snapshots
// {id, name, description, mediaClass}: inputs, then Backline instruments,
// then outputs (recorded from their monitor). Monitors, streams of other apps
// and Backline's own recorder and soundcheck nodes are left out.
function sourceKind(node) {
  var name = String(node.name || "")
  var cls = String(node.mediaClass || "")
  if (name.indexOf("backline.recorder") === 0 || name.indexOf("backline.soundcheck") === 0 || name === "backline.clock") return ""
  if (name.indexOf("backline.") === 0 && cls.indexOf("Stream/Output/Audio") === 0) return "instrument"
  if (cls.indexOf("Audio/Source") === 0) return "input"
  if (cls === "Audio/Sink") return "output"
  return ""
}

var KIND_ORDER = { input: 0, instrument: 1, output: 2 }

function recordSources(nodes, filter) {
  var re = filter ? new RegExp(filter) : null
  var list = []
  for (var i = 0; i < (nodes || []).length; i++) {
    var n = nodes[i]
    var kind = sourceKind(n)
    if (!kind) continue
    if (re && !re.test(String(n.name))) continue
    list.push({ id: n.id, name: String(n.name), label: String(n.description || n.name), kind: kind })
  }
  list.sort(function(a, b) {
    if (KIND_ORDER[a.kind] !== KIND_ORDER[b.kind]) return KIND_ORDER[a.kind] - KIND_ORDER[b.kind]
    return a.label.toLowerCase() < b.label.toLowerCase() ? -1 : 1
  })
  return list
}

function clock(seconds) {
  var s = Math.max(0, Math.floor(seconds || 0))
  var h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, r = s % 60
  function two(v) { return v < 10 ? "0" + v : String(v) }
  return two(h) + ":" + two(m) + ":" + two(r)
}

function shortLength(seconds) {
  var s = Math.max(0, Math.round(seconds || 0))
  var m = Math.floor(s / 60), r = s % 60
  return m + ":" + (r < 10 ? "0" + r : r)
}

function recordHero(model, seconds, tr) {
  var t = pick(tr)
  if (!model) return t("record.idle", "READY")
  var tracks = (model.armed || []).length
  if (model.state === "recording") return fill(t("record.heroRecording", "RECORDING %1"), [clock(seconds)])
  if (model.state === "armed")
    return tracks === 1 ? t("record.heroArmedOne", "ARMED · 1 TRACK") : fill(t("record.heroArmed", "ARMED · %1 TRACKS"), [tracks])
  if (model.soundcheck) return t("record.heroSoundcheck", "SOUNDCHECK")
  return t("record.idle", "READY")
}

// ---------------------------------------------------------------- transport

// HH:MM:SS:FF of a frame at a rate and timecode fps; 29.97 is drop-frame
// (frame numbers 0 and 1 skipped each minute except every tenth).
function timecode(frame, rate, fps) {
  if (!rate || rate <= 0) return "--:--:--:--"
  var drop = Math.abs(fps - 29.97) < 0.01
  var nominal = Math.round(fps)
  var frames = Math.floor((frame / rate) * fps + 1e-6)
  if (drop) {
    var perTen = 17982, perMinute = 1798
    var tens = Math.floor(frames / perTen), rem = frames % perTen
    frames += 18 * tens + (rem > 1 ? 2 * Math.floor((rem - 2) / perMinute) : 0)
  }
  var ff = frames % nominal
  var total = Math.floor(frames / nominal)
  function two(v) { return v < 10 ? "0" + v : String(v) }
  return two(Math.floor(total / 3600) % 24) + ":" + two(Math.floor(total / 60) % 60) + ":" + two(total % 60) + (drop ? ";" : ":") + two(ff)
}

// Where the transport is now, from the last report and the time since it.
function projectedFrame(report, nowMs) {
  if (!report) return 0
  var frame = Number(report.frame || 0)
  if (!report.rolling || !report.rate) return frame
  return frame + Math.max(0, (nowMs - report.atMs) / 1000) * report.rate
}

var SYNC_SOURCES = ["internal", "link", "ltc", "mtc"]

function syncLabel(source, tr) {
  var t = pick(tr)
  if (source === "link") return t("transport.link", "Link")
  if (source === "ltc") return t("transport.ltc", "LTC")
  if (source === "mtc") return t("transport.mtc", "MTC")
  return t("transport.internal", "Internal")
}

// The chase status line and its tone: locked (ok), freewheeling (warn: was
// locked, signal lost), or no signal yet (muted).
function chaseStatus(clockState, tr) {
  var t = pick(tr)
  var c = clockState || {}
  var chasing = c.LtcMode === "chase" || c.MtcMode === "chase"
  if (!chasing) {
    if (c.LinkEnabled) {
      var peers = Number(c.LinkPeers || 0)
      return { text: peers === 1 ? t("transport.peerOne", "1 LINK PEER") : fill(t("transport.peers", "%1 LINK PEERS"), [peers]), tone: peers > 0 ? "ok" : "muted" }
    }
    return { text: "", tone: "muted" }
  }
  if (c.Locked) return { text: fill(t("transport.locked", "LOCKED · %1 FR"), [Number(c.ChaseError || 0)]), tone: "ok" }
  if (c.Rolling) return { text: t("transport.freewheel", "FREEWHEEL"), tone: "warn" }
  return { text: t("transport.noSignal", "NO SIGNAL"), tone: "muted" }
}

var FPS_CHOICES = [24, 25, 29.97, 30]

if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    parseJson: parseJson, gradeRank: gradeRank, sortChoices: sortChoices, matches: matches,
    filterChoices: filterChoices, midiIndex: midiIndex, midiLabel: midiLabel,
    instrumentStatus: instrumentStatus, statusTone: statusTone, instrumentsHero: instrumentsHero,
    PLUGIN_PILLS: PLUGIN_PILLS, filterProducts: filterProducts, formatsLine: formatsLine,
    findingLine: findingLine, gradeLabel: gradeLabel, sourceKind: sourceKind,
    recordSources: recordSources, clock: clock, shortLength: shortLength, recordHero: recordHero,
    timecode: timecode, projectedFrame: projectedFrame, SYNC_SOURCES: SYNC_SOURCES,
    syncLabel: syncLabel, chaseStatus: chaseStatus, FPS_CHOICES: FPS_CHOICES
  }
}
