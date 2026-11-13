// Pure logic shared by Backline's panels (Instruments, Record, Plugins,
// Transport, Audio Setup). Plain JavaScript with no QML, so Node can load it for tests;
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

// ------------------------------------------------------------- audio setup
// Pro audio (omarchy-backline-pro), combined devices
// (omarchy-backline-aggregate) and MIDI ports (omarchy-backline-midi): the
// panel offers the buffer sizes and rates a session picks between; the
// commands take others.

var PRO_BUFFERS = [64, 128, 256, 512, 1024]
var PRO_RATES = [44100, 48000, 88200, 96000]

// Numbers go through the shell's I18n.formatNumber when QML passes it; Node
// tests leave it out and get plain English digits.
function formatter(fmt) {
  return typeof fmt === "function" ? fmt : function(v, d) { return d === undefined ? String(v) : Number(v).toFixed(d) }
}

// `omarchy-backline-pro status --json`, or null when PipeWire isn't answering
// (the command then prints nothing on stdout) or the output is not its own.
function parseProStatus(text) {
  var data = parseJson(text, null)
  if (!data || typeof data !== "object" || Array.isArray(data)) return null
  var buffer = Number(data.buffer)
  var rate = Number(data.rate)
  if (!(buffer > 0) || !(rate > 0)) return null
  return { enabled: data.enabled === true, buffer: buffer, rate: rate, realtime: data.realtime === true }
}

function proLatency(buffer, rate, fmt) {
  return formatter(fmt)(buffer / rate * 1000, 1)
}

// "PRO AUDIO", and once it is on the time one buffer takes: "PRO AUDIO · 2.7 ms".
function proTitle(status, tr, fmt) {
  var t = pick(tr)
  if (!status || !status.enabled) return t("setup.pro", "PRO AUDIO")
  return fill(t("setup.proLatency", "PRO AUDIO · %1 ms"), [proLatency(status.buffer, status.rate, fmt)])
}

// 44100 -> "44.1", 48000 -> "48": the pill. The tooltip adds the unit.
function rateLabel(rate, fmt) {
  var khz = rate / 1000
  return khz % 1 === 0 ? formatter(fmt)(khz) : formatter(fmt)(khz, 1)
}

function rateTooltip(rate, tr, fmt) {
  return fill(pick(tr)("setup.kiloHertz", "%1 kHz"), [rateLabel(rate, fmt)])
}

function bufferTooltip(buffer, rate, tr, fmt) {
  return fill(pick(tr)("setup.bufferTooltip", "%1 frames, %2 ms"), [formatter(fmt)(buffer), proLatency(buffer, rate, fmt)])
}

// Where the cursor lands in a row of pills: on the value in force, else the first.
function pillIndex(values, value) {
  var index = (values || []).indexOf(value)
  return index >= 0 ? index : 0
}

// `omarchy-backline-aggregate list --json`: the devices combined here.
function parseCombined(text) {
  var data = parseJson(text, [])
  if (!Array.isArray(data)) return []
  var rows = []
  for (var i = 0; i < data.length; i++) {
    var d = data[i]
    if (!d || typeof d !== "object" || !d.name) continue
    rows.push({ name: String(d.name), active: d.active === true, mirror: d.mirror === true })
  }
  return rows
}

// "Multi-output · running", "Aggregate · stopped", and while one of its
// devices is being converted, "Multi-output · running · SRC 48 → 44.1 kHz".
function combinedCaption(device, tr, conversions, fmt) {
  var t = pick(tr)
  var kind = device.mirror ? t("setup.multiOutput", "Multi-output") : t("setup.aggregate", "Aggregate")
  var parts = [kind, device.active ? t("setup.running", "running") : t("setup.stopped", "stopped")]
  var src = []
  for (var i = 0; i < (conversions || []).length; i++) {
    var c = conversions[i]
    if (c.combined !== device.name) continue
    var label = srcLabel(c, tr, fmt)
    if (src.indexOf(label) < 0) src.push(label)
  }
  return parts.concat(src).join(" · ")
}

// ------------------------------------------------- sample-rate conversion

// `omarchy-backline-src --json`: each stream PipeWire converts, with the
// rates on either side. None when PipeWire is down or the output is not its own.
function parseConversions(text) {
  var data = parseJson(text, [])
  if (!Array.isArray(data)) return []
  var rows = []
  for (var i = 0; i < data.length; i++) {
    var c = data[i]
    if (!c || typeof c !== "object") continue
    var from = Number(c.from), to = Number(c.to)
    if (!(from > 0) || !(to > 0) || from === to) continue
    rows.push({ name: String(c.name || c.node || ""), from: from, to: to,
      direction: c.direction === "capture" ? "capture" : "playback",
      deviceName: String(c.deviceName || c.device || ""),
      internal: c.internal === true, combined: c.combined ? String(c.combined) : null })
  }
  return rows
}

// "SRC 44.1 → 48 kHz": the conversion in the direction the audio flows.
function srcLabel(conversion, tr, fmt) {
  var t = pick(tr)
  return fill(t("setup.src", "SRC %1 → %2 kHz"), [rateLabel(conversion.from, fmt), rateLabel(conversion.to, fmt)])
}

// The quiet lines under the sample rate, one per app being converted:
// "SRC 44.1 → 48 kHz · Spotify". Filters' and combined devices' own streams are
// left out; a combined device shows its conversion on its own row.
function srcLines(conversions, tr, fmt) {
  var lines = []
  for (var i = 0; i < (conversions || []).length; i++) {
    var c = conversions[i]
    if (c.internal) continue
    var line = c.name ? srcLabel(c, tr, fmt) + " · " + c.name : srcLabel(c, tr, fmt)
    if (lines.indexOf(line) < 0) lines.push(line)
  }
  return lines
}

// `omarchy-backline-midi list --json`: the sequencer's ports, or none when the
// output is not its own.
function parseMidiPorts(text) {
  var data = parseJson(text, [])
  if (!Array.isArray(data)) return []
  var ports = []
  for (var i = 0; i < data.length; i++) {
    var p = data[i]
    if (!p || typeof p !== "object" || !p.address) continue
    ports.push({
      address: String(p.address),
      client: String(p.client || ""),
      port: String(p.port || ""),
      kind: String(p.kind || ""),
      protocol: String(p.protocol || ""),
      direction: String(p.direction || ""),
      sendsTo: Array.isArray(p.sendsTo) ? p.sendsTo.map(String) : []
    })
  }
  return ports
}

// The ports a person plays or patches from: devices and apps that send. Kernel
// buses like Midi Through are always there and would only be noise.
function midiSources(ports) {
  var list = (ports || []).filter(function(p) {
    return (p.kind === "hardware" || p.kind === "app") && (p.direction === "source" || p.direction === "both")
  })
  list.sort(function(a, b) {
    if (a.kind !== b.kind) return a.kind === "hardware" ? -1 : 1
    return String(a.address).localeCompare(String(b.address), undefined, { numeric: true })
  })
  return list
}

// Hardware names its ports after the device ("Launchkey MK3 MIDI 1"); apps
// often do not ("output"), so the client leads where the port leaves it out.
function portLabel(port) {
  if (!port.port || port.port.indexOf(port.client) >= 0) return port.port || port.client
  return port.client + " " + port.port
}

// "MIDI 2.0 · to Surge XT, Vital", naming each destination by its client.
function portCaption(port, ports, tr) {
  var t = pick(tr)
  var names = []
  for (var i = 0; i < port.sendsTo.length; i++) {
    var name = port.sendsTo[i]
    for (var j = 0; j < (ports || []).length; j++)
      if (ports[j].address === port.sendsTo[i]) name = ports[j].client
    if (names.indexOf(name) < 0) names.push(name)
  }
  var where = names.length > 0 ? fill(t("setup.sendsTo", "to %1"), [names.join(", ")]) : t("setup.unconnected", "not connected")
  return port.protocol + " · " + where
}

// The panel's one cursor walks these rows, top to bottom: the pro audio
// switch, its two pill rows while it is on, each combined device, the add
// row, then each MIDI port that sends. A section PipeWire or the sequencer
// isn't answering for has no rows.
function setupRows(proStatus, combined, midiRows) {
  var rows = []
  if (proStatus) {
    rows.push({ kind: "pro" })
    if (proStatus.enabled) rows.push({ kind: "buffer" }, { kind: "rate" })
  }
  for (var i = 0; i < (combined || []).length; i++) rows.push({ kind: "device", index: i })
  rows.push({ kind: "add" })
  for (var j = 0; j < (midiRows || []).length; j++) rows.push({ kind: "midi", index: j })
  return rows
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    parseJson: parseJson, gradeRank: gradeRank, sortChoices: sortChoices, matches: matches,
    filterChoices: filterChoices, midiIndex: midiIndex, midiLabel: midiLabel,
    instrumentStatus: instrumentStatus, statusTone: statusTone, instrumentsHero: instrumentsHero,
    PLUGIN_PILLS: PLUGIN_PILLS, filterProducts: filterProducts, formatsLine: formatsLine,
    findingLine: findingLine, gradeLabel: gradeLabel, sourceKind: sourceKind,
    recordSources: recordSources, clock: clock, shortLength: shortLength, recordHero: recordHero,
    timecode: timecode, projectedFrame: projectedFrame, SYNC_SOURCES: SYNC_SOURCES,
    syncLabel: syncLabel, chaseStatus: chaseStatus, FPS_CHOICES: FPS_CHOICES,
    PRO_BUFFERS: PRO_BUFFERS, PRO_RATES: PRO_RATES, parseProStatus: parseProStatus, proTitle: proTitle,
    rateLabel: rateLabel, rateTooltip: rateTooltip, bufferTooltip: bufferTooltip, pillIndex: pillIndex,
    parseCombined: parseCombined, combinedCaption: combinedCaption, parseConversions: parseConversions,
    srcLabel: srcLabel, srcLines: srcLines, parseMidiPorts: parseMidiPorts,
    midiSources: midiSources, portLabel: portLabel, portCaption: portCaption, setupRows: setupRows
  }
}
