import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import Quickshell.Services.Pipewire
import qs.Ui
import qs.Commons
import qs.Commons as Commons
import "Model.js" as Model

// Transport: the system transport Backline's clock keeps on PipeWire's own
// timeline (what JACK apps and Backline instruments follow), its tempo, and
// what moves it: internal, Ableton Link, or chased LTC or MTC. Locking to
// timecode is the SYNC pills plus, the first time, an input. The clock's
// Transport signal arrives every 250 ms while rolling; between signals the
// timecode is projected from the graph rate, only while the panel is open.
// The bar shows the timecode and the lock while rolling or chasing.
Panel {
  id: root
  moduleName: "backline.transport"
  ipcTarget: "backline.transport"
  manageIpc: false

  property QtObject strings: Commons.Translations { directory: Qt.resolvedUrl("i18n") }
  function t(key, english) { return root.strings.t(key, english) }

  property var model: ({ running: false, clock: {}, sync: { ltc: "", mtc: "", fps: 25 } })
  readonly property var clock: model.clock || {}
  property bool stateStale: false
  property var report: ({ rolling: false, frame: 0, rate: 0, atMs: 0, bbt: "" })
  property real nowMs: 0

  readonly property bool rolling: !!clock.Rolling
  readonly property string source: clock.Source || "internal"
  readonly property bool chasing: clock.LtcMode === "chase" || clock.MtcMode === "chase"
  readonly property real fps: Number(clock.Fps || model.sync.fps || 25)
  readonly property string timecodeText: model.running
    ? Model.timecode(Model.projectedFrame(report, nowMs), report.rate || clock.Rate || 0, fps)
    : "--:--:--:--"
  readonly property var chase: Model.chaseStatus(clock, root.t)

  // ---- Cursor ----
  // rows: tempo, sync pills, then (chasing LTC or MTC) input pills and fps.
  readonly property string chaseKind: clock.LtcMode === "chase" ? "ltc" : (clock.MtcMode === "chase" ? "mtc" : "")
  readonly property var rows: chaseKind !== "" ? ["tempo", "sync", "input", "fps"] : ["tempo", "sync"]
  property int selectedIndex: 1
  property int syncPill: 0
  readonly property string currentRow: rows[Math.max(0, Math.min(rows.length - 1, selectedIndex))]

  // Timecode inputs: audio sources for LTC (anything with audio outputs),
  // MIDI bridges for MTC. Primitive snapshots, as everywhere.
  property var inputChoices: []
  function snapshotInputs() {
    var nodes = Pipewire.nodes ? Pipewire.nodes.values : []
    var filter = Quickshell.env("BACKLINE_NODE_FILTER") || ""
    var re = filter ? new RegExp(filter) : null
    var list = []
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i]
      if (!n) continue
      var name = String(n.name || "")
      if (re && !re.test(name)) continue
      if (name.indexOf("backline.") === 0) continue
      var audioSource = !!n.audio && !n.isStream && !n.isSink
      var audioSink = !!n.audio && !n.isStream && n.isSink
      var midi = !n.audio && name.indexOf("Midi") >= 0
      if (chaseKind === "mtc" ? midi : (audioSource || audioSink))
        list.push({ name: name, label: String(n.description || n.nickname || name) })
    }
    inputChoices = list
  }
  readonly property string chaseInput: chaseKind === "" ? "" : (model.sync[chaseKind] || "")

  function moveCursor(dy) {
    selectedIndex = Math.max(0, Math.min(rows.length - 1, selectedIndex + dy))
    if (currentRow === "sync") syncPill = Math.max(0, Model.SYNC_SOURCES.indexOf(source))
  }

  function sideways(dx) {
    if (currentRow === "tempo") {
      run(["omarchy-backline-transport", "tempo", String(Math.max(20, Math.min(999, Math.round(Number(clock.Tempo || 120)) + dx)))])
    } else if (currentRow === "sync") {
      syncPill = Math.max(0, Math.min(Model.SYNC_SOURCES.length - 1, syncPill + dx))
    } else if (currentRow === "input" && inputChoices.length > 0) {
      var i = 0
      for (var k = 0; k < inputChoices.length; k++) if (inputChoices[k].name === chaseInput) i = k
      var next = inputChoices[Math.max(0, Math.min(inputChoices.length - 1, i + dx))]
      if (next && next.name !== chaseInput) run(["omarchy-backline-transport", chaseKind + "-input", next.name])
    } else if (currentRow === "fps") {
      var f = 0
      for (var j = 0; j < Model.FPS_CHOICES.length; j++) if (Math.abs(Model.FPS_CHOICES[j] - fps) < 0.01) f = j
      var nf = Model.FPS_CHOICES[Math.max(0, Math.min(Model.FPS_CHOICES.length - 1, f + dx))]
      if (Math.abs(nf - fps) > 0.001) run(["omarchy-backline-transport", "fps", String(nf)])
    }
  }

  function activate() {
    if (currentRow === "sync") {
      var wanted = Model.SYNC_SOURCES[syncPill]
      if (wanted !== source) run(["omarchy-backline-transport", "sync", wanted])
    } else {
      playStop()
    }
  }

  function playStop() { run(["omarchy-backline-transport", "toggle"]) }

  // ---- State ----
  function refresh() {
    if (stateProc.running) { stateStale = true; return }
    stateProc.running = true
  }

  function applyState(text) {
    var next = Model.parseJson(text, null)
    if (!next) return
    var previousSource = root.source
    model = next
    var c = next.clock || {}
    report = { rolling: !!c.Rolling, frame: Number(c.Frame || 0), rate: Number(c.Rate || 0), atMs: Date.now(), bbt: String(c.Bbt || "") }
    // The pill cursor follows the source only when the source changes, so a
    // refresh arriving between two key presses can't move it back.
    if (!opened || (c.Source || "internal") !== previousSource) syncPill = Math.max(0, Model.SYNC_SOURCES.indexOf(c.Source || "internal"))
    if (selectedIndex > rows.length - 1) selectedIndex = rows.length - 1
    if (opened) snapshotInputs()
  }

  function onSignal(line) {
    var msg = Model.parseJson(line, null)
    if (!msg || msg.service !== "clock") return
    if (msg.member === "Transport" && msg.args && msg.args.length >= 4) {
      report = { rolling: !!msg.args[0], frame: Number(msg.args[1]), rate: report.rate || Number(clock.Rate || 0), atMs: Date.now(), bbt: String(msg.args[3]) }
      if (!!msg.args[0] !== rolling) refreshTimer.restart()
      return
    }
    refreshTimer.restart()
  }

  function run(command) {
    if (actionProc.running) return
    actionProc.command = command
    actionProc.running = true
  }

  onOpenedChanged: {
    if (opened) {
      selectedIndex = 1
      syncPill = Math.max(0, Model.SYNC_SOURCES.indexOf(source))
      refresh()
    }
  }

  Process {
    id: stateProc
    command: ["omarchy-backline-state", "transport"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.applyState(text)
    }
    onExited: if (root.stateStale) { root.stateStale = false; Qt.callLater(root.refresh) }
  }

  Process {
    id: actionProc
    onExited: root.refresh()
  }

  Process {
    id: watchProc
    running: root.opened || root.rolling || root.chasing
    command: ["omarchy-backline-watch"]
    stdout: SplitParser { onRead: function(line) { root.onSignal(line) } }
  }

  Timer { id: refreshTimer; interval: 60; onTriggered: root.refresh() }

  // The projected timecode, at the timecode rate, only while it is on screen
  // and moving.
  Timer {
    interval: 40
    repeat: true
    running: root.opened && root.report.rolling
    onTriggered: root.nowMs = Date.now()
  }

  Component.onCompleted: refresh()

  ShellIpc {
    target: "backline.transport"
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): void { root.refresh() }
    function state(): string {
      return JSON.stringify({ opened: root.opened, row: root.currentRow, syncPill: Model.SYNC_SOURCES[root.syncPill],
        running: root.model.running, rolling: root.rolling, source: root.source, locked: !!root.clock.Locked,
        timecode: root.timecodeText, chase: root.chase.text, input: root.chaseInput, fps: root.fps })
    }
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  // A label (count, timecode) widens the slot by its own width, so it never
  // runs into the next widget.
  TextMetrics {
    id: labelMetrics
    font.family: root.bar ? root.bar.fontFamily : Style.font.family
    font.pixelSize: Style.bar.iconFont
    text: button.labelText
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    fixedWidth: root.bar && root.bar.vertical ? -1
      : (labelText !== "" ? Style.bar.iconSlot + Math.ceil(labelMetrics.advanceWidth) + Style.space(6) : Style.bar.iconSlot)
    text: root.chasing ? (root.clock.Locked ? "󰌾" : "󰍁") : (root.rolling ? "󰐊" : "󰔛")
    labelText: (root.rolling || root.chasing) && root.model.running ? root.timecodeText : ""
    active: root.chasing && !root.clock.Locked
    tooltipText: root.t("transport.title", "Transport")
    onPressed: function(b) { if (b === Qt.RightButton) root.playStop(); else root.toggle() }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(400))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight, Style.space(560))

    content: PanelKeyCatcher {
      id: keyCatcher
      property bool viaReturn: false
      implicitHeight: column.implicitHeight
      anchors.fill: parent
      onMoveRequested: function(dx, dy) {
        if (dy !== 0) root.moveCursor(dy)
        else root.sideways(dx)
      }
      // Enter acts on the cursor; Space is play/stop wherever the cursor is.
      onReturnRequested: viaReturn = true
      onActivateRequested: {
        if (viaReturn) root.activate()
        else root.playStop()
        viaReturn = false
      }
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      onTextKey: function(t) {
        if (t === "0") root.run(["omarchy-backline-transport", "locate", "0"])
        else if (t === "o") Quickshell.execDetached(["omarchy-backline-mixer"])
      }

      Column {
        id: column
        width: parent.width
        spacing: Style.space(12)

        // ---- Hero: timecode, bars|beats, tempo; play/stop ----
        Item {
          width: parent.width
          implicitHeight: Math.max(heroLabels.implicitHeight, playButton.implicitHeight)

          Column {
            id: heroLabels
            anchors.left: parent.left
            anchors.right: playButton.left
            anchors.rightMargin: Style.space(8)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            Text {
              textFormat: Text.PlainText
              text: root.timecodeText
              color: root.chasing && root.clock.Locked ? root.bar.foreground : (root.rolling ? root.bar.foreground : Qt.darker(root.bar.foreground, 1.2))
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.displayLarge
              font.bold: true
              font.features: { "tnum": 1 }
            }

            Text {
              textFormat: Text.PlainText
              text: root.model.running
                ? [String(root.report.bbt || root.clock.Bbt || ""),
                   root.t("transport.bpm", "%1 BPM").arg(Commons.I18n.formatNumber(Number(root.clock.Tempo || 0), 1)),
                   Number(root.clock.BeatsPerBar || 4) + "/" + Number(root.clock.BeatType || 4),
                   Model.syncLabel(root.source, root.t).toUpperCase()].join("  ·  ")
                : root.t("transport.notRunning", "CLOCK STOPPED · SPACE STARTS IT")
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption
              font.bold: true
              font.letterSpacing: 1.1
              width: parent.width
              elide: Text.ElideRight
            }
          }

          Button {
            id: playButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            iconText: root.rolling ? "󰓛" : "󰐊"
            text: root.rolling ? root.t("transport.stop", "Stop") : root.t("transport.play", "Play")
            active: root.rolling
            enabled: !root.chasing
            foreground: root.bar.foreground
            fontFamily: root.bar.fontFamily
            tooltipText: root.t("transport.playHint", "Play or stop (Space)")
            onClicked: root.playStop()
          }
        }

        PanelSeparator { foreground: root.bar.foreground }

        // ---- Tempo ----
        CursorSurface {
          id: tempoRow
          width: parent.width
          implicitHeight: tempoLabel.implicitHeight + Style.spacing.lg
          hasCursor: root.currentRow === "tempo"
          foreground: root.bar.foreground
          Text {
            id: tempoLabel
            textFormat: Text.PlainText
            text: root.t("transport.tempo", "TEMPO")
            color: root.bar.foreground
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
            font.bold: true
            font.letterSpacing: 1.2
            anchors.left: parent.left
            anchors.leftMargin: Style.space(6)
            anchors.verticalCenter: parent.verticalCenter
          }
          Text {
            textFormat: Text.PlainText
            text: "‹  " + Commons.I18n.formatNumber(Number(root.clock.Tempo || 0), 1) + "  ›"
            color: root.bar.foreground
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
            font.bold: true
            anchors.right: parent.right
            anchors.rightMargin: Style.space(6)
            anchors.verticalCenter: parent.verticalCenter
          }
          HoverHandler { onHoveredChanged: if (hovered) root.selectedIndex = 0 }
        }

        // ---- Sync: what moves the transport, and the chase status ----
        CursorSurface {
          width: parent.width
          implicitHeight: syncColumn.implicitHeight + Style.spacing.lg
          hasCursor: root.currentRow === "sync"
          foreground: root.bar.foreground

          Column {
            id: syncColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Style.space(6)
            anchors.rightMargin: Style.space(6)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(6)

            Item {
              width: parent.width
              implicitHeight: syncHeader.implicitHeight
              PanelSectionHeader {
                id: syncHeader
                text: root.t("transport.sync", "SYNC")
                foreground: root.bar.foreground
                fontFamily: root.bar.fontFamily
              }
              Text {
                textFormat: Text.PlainText
                text: root.chase.text
                color: root.chase.tone === "ok" ? Commons.Color.accent : (root.chase.tone === "warn" ? Commons.Color.urgent : Qt.darker(root.bar.foreground, 1.5))
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                font.letterSpacing: 1.1
                anchors.right: parent.right
                anchors.verticalCenter: syncHeader.verticalCenter
              }
            }

            Row {
              spacing: Style.space(4)
              Repeater {
                model: Model.SYNC_SOURCES
                Button {
                  required property var modelData
                  required property int index
                  text: Model.syncLabel(modelData, root.t)
                  selected: modelData === root.source
                  hasCursor: root.currentRow === "sync" && index === root.syncPill
                  foreground: root.bar.foreground
                  fontFamily: root.bar.fontFamily
                  fontSize: Style.font.caption
                  onHovered: function(on) { if (on) { root.selectedIndex = 1; root.syncPill = index } }
                  onClicked: { root.selectedIndex = 1; root.syncPill = index; root.activate() }
                }
              }
            }
          }
        }

        // ---- Chase input and frame rate ----
        CursorSurface {
          visible: root.chaseKind !== ""
          width: parent.width
          implicitHeight: inputColumn.implicitHeight + Style.spacing.lg
          hasCursor: root.currentRow === "input"
          foreground: root.bar.foreground
          Column {
            id: inputColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Style.space(6)
            anchors.rightMargin: Style.space(6)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(6)
            PanelSectionHeader {
              text: root.chaseKind === "mtc" ? root.t("transport.mtcInput", "MTC FROM") : root.t("transport.ltcInput", "LTC FROM")
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
            }
            Flow {
              width: parent.width
              spacing: Style.space(4)
              Repeater {
                model: root.inputChoices
                Button {
                  required property var modelData
                  text: modelData.label
                  selected: modelData.name === root.chaseInput
                  foreground: root.bar.foreground
                  fontFamily: root.bar.fontFamily
                  fontSize: Style.font.caption
                  onClicked: root.run(["omarchy-backline-transport", root.chaseKind + "-input", modelData.name])
                }
              }
            }
          }
          HoverHandler { onHoveredChanged: if (hovered) root.selectedIndex = 2 }
        }

        CursorSurface {
          visible: root.chaseKind !== ""
          width: parent.width
          implicitHeight: fpsRow.implicitHeight + Style.spacing.lg
          hasCursor: root.currentRow === "fps"
          foreground: root.bar.foreground
          Row {
            id: fpsRow
            anchors.left: parent.left
            anchors.leftMargin: Style.space(6)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(4)
            PanelSectionHeader {
              text: root.t("transport.fps", "FPS")
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
              anchors.verticalCenter: parent.verticalCenter
              width: Style.space(48)
            }
            Repeater {
              model: Model.FPS_CHOICES
              Button {
                required property var modelData
                text: modelData === 29.97 ? "29.97 DF" : String(modelData)
                selected: Math.abs(modelData - root.fps) < 0.01
                foreground: root.bar.foreground
                fontFamily: root.bar.fontFamily
                fontSize: Style.font.caption
                onClicked: root.run(["omarchy-backline-transport", "fps", String(modelData)])
              }
            }
          }
          HoverHandler { onHoveredChanged: if (hovered) root.selectedIndex = 3 }
        }

        Text {
          textFormat: Text.PlainText
          text: root.t("transport.keys", "Space plays · h/l tempo or pills · Enter applies · 0 to start")
          color: Qt.darker(root.bar.foreground, 1.6)
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
          width: parent.width
          wrapMode: Text.Wrap
        }
      }
    }
  }
}
