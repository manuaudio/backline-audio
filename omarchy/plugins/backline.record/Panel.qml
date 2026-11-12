import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import Quickshell.Services.Pipewire
import qs.Ui
import qs.Commons
import qs.Commons as Commons
import "Model.js" as Model

// Record: arm sources (inputs, Backline instruments, outputs), record them
// into one sample-aligned take, drop markers, and play a take back in place
// of its inputs (virtual soundcheck). Arming arms Backline's recorder at once
// with pre-roll, so `r` captures the seconds before it was pressed. Sources
// and their meters come from PipeWire through Quickshell; the recorder's state
// from omarchy-backline-state on open and on every recorder signal while open,
// and while recording, so the bar icon stays red exactly as long as the take.
Panel {
  id: root
  moduleName: "backline.record"
  ipcTarget: "backline.record"
  manageIpc: false

  property QtObject strings: Commons.Translations { directory: Qt.resolvedUrl("i18n") }
  function t(key, english) { return root.strings.t(key, english) }

  property var model: ({ running: false, state: "idle", armed: [], takes: [], inputs: {}, soundcheck: "", rate: 0 })
  property bool stateStale: false
  property string busyText: ""
  property real recordedSeconds: 0
  readonly property string nodeFilter: Quickshell.env("BACKLINE_NODE_FILTER") || ""

  readonly property bool recording: model.state === "recording"
  readonly property bool soundchecking: (model.soundcheck || "") !== ""

  // Rows are primitive snapshots of the PipeWire nodes, refreshed after the
  // graph settles; delegates resolve their node by id and name when they
  // need one (the audio panel's rule: a row never holds a PwNode).
  property var sources: []
  function snapshotSources() {
    var nodes = Pipewire.nodes ? Pipewire.nodes.values : []
    var list = []
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i]
      if (!n) continue
      var name = String(n.name || "")
      if (!n.audio && name.indexOf("backline.") !== 0) continue
      var cls = n.isStream ? (n.isSink ? "Stream/Output/Audio" : "Stream/Input/Audio") : (n.isSink ? "Audio/Sink" : "Audio/Source")
      list.push({ id: n.id, name: name, description: String(n.description || n.nickname || name), mediaClass: cls })
    }
    sources = Model.recordSources(list, nodeFilter)
  }

  function nodeFor(row) {
    var nodes = Pipewire.nodes ? Pipewire.nodes.values : []
    for (var i = 0; i < nodes.length; i++)
      if (nodes[i] && nodes[i].id === row.id && String(nodes[i].name || "") === row.name) return nodes[i]
    return null
  }

  readonly property var takes: (model.takes || []).slice(0, 5)
  readonly property var inputs: {
    var list = []
    var map = model.inputs || {}
    for (var k in map) list.push({ node: k, mode: map[k] })
    return list
  }

  // ---- Cursor: one flat list of rows across the sections ----
  readonly property var rows: {
    var list = []
    for (var i = 0; i < sources.length; i++) list.push({ type: "source", index: i })
    if (soundchecking) {
      for (var j = 0; j < inputs.length; j++) list.push({ type: "input", index: j })
      list.push({ type: "stop", index: 0 })
    }
    for (var k = 0; k < takes.length; k++) list.push({ type: "take", index: k })
    return list
  }
  property int selectedIndex: 0
  readonly property var current: rows[Math.max(0, Math.min(rows.length - 1, selectedIndex))] || null
  function isCurrent(type, index) { return !!current && current.type === type && current.index === index }
  function select(type, index) {
    for (var i = 0; i < rows.length; i++) if (rows[i].type === type && rows[i].index === index) { selectedIndex = i; return }
  }

  function moveCursor(dy) { selectedIndex = Math.max(0, Math.min(rows.length - 1, selectedIndex + dy)) }

  function isArmed(name) { return (model.armed || []).indexOf(name) >= 0 }

  function activate() {
    if (!current) return
    if (current.type === "source") run(["omarchy-backline-record", "source", sources[current.index].name, "toggle"])
    else if (current.type === "take") run(["omarchy-backline-soundcheck", "start", takes[current.index].take])
    else if (current.type === "input") setInput(inputs[current.index].node, inputs[current.index].mode === "live" ? "playback" : "live")
    else if (current.type === "stop") run(["omarchy-backline-soundcheck", "stop"])
  }

  // h is live, l is playback, on a soundcheck input.
  function sideways(dx) {
    if (!current || current.type !== "input") return
    var row = inputs[current.index]
    var mode = dx > 0 ? "playback" : "live"
    if (row.mode !== mode) setInput(row.node, mode)
  }

  function setInput(node, mode) { run(["omarchy-backline-soundcheck", "input", node, mode]) }

  function textKey(t) {
    if (t === "r") run(["omarchy-backline-record", "toggle"])
    else if (t === "m" && recording) run(["omarchy-backline-record", "marker"])
    else if (t === "p") run(["omarchy-backline-soundcheck", "toggle"])
    else if (t === "a") {
      if (soundchecking) run(["omarchy-backline-soundcheck", "all", "playback"])
      else run(["omarchy-backline-record", "arm"].concat(sources.map(function(s) { return s.name })))
    }
    else if (t === "o") Quickshell.execDetached(["omarchy-backline-mixer"])
  }

  // ---- State ----
  function refresh() {
    if (stateProc.running) { stateStale = true; return }
    stateProc.running = true
  }

  function applyState(text) {
    var next = Model.parseJson(text, null)
    if (!next) return
    if (next.state !== "recording") recordedSeconds = 0
    var startedSoundcheck = !(model.soundcheck) && !!next.soundcheck
    model = next
    // A soundcheck just loaded: the cursor goes to its first input, so `l`
    // puts it on playback.
    if (startedSoundcheck) select("input", 0)
    if (selectedIndex > rows.length - 1) selectedIndex = Math.max(0, rows.length - 1)
  }

  function run(command) {
    if (actionProc.running) return
    actionProc.command = command
    actionProc.running = true
  }

  // The recorder's Level signal carries the frames recorded so far.
  function onSignal(line) {
    var msg = Model.parseJson(line, null)
    if (!msg || msg.service !== "recorder") return
    if (msg.member === "Level") {
      if (root.model.rate > 0 && msg.args && msg.args.length > 0) root.recordedSeconds = Number(msg.args[0]) / root.model.rate
      return
    }
    refreshTimer.restart()
  }

  onOpenedChanged: {
    if (opened) {
      snapshotSources()
      selectedIndex = 0
      refresh()
    } else {
      sources = []
    }
  }

  onSourcesChanged: if (selectedIndex > rows.length - 1) selectedIndex = Math.max(0, rows.length - 1)

  Connections {
    target: Pipewire.nodes
    function onValuesChanged() { if (root.opened) snapshotTimer.restart() }
  }

  Timer { id: snapshotTimer; interval: 120; onTriggered: root.snapshotSources() }

  Process {
    id: stateProc
    command: ["omarchy-backline-state", "record"]
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

  // Open, or while a take or soundcheck runs (so the bar shows when it ends).
  Process {
    id: watchProc
    running: root.opened || root.recording || root.soundchecking
    command: ["omarchy-backline-watch"]
    stdout: SplitParser { onRead: function(line) { root.onSignal(line) } }
  }

  Timer { id: refreshTimer; interval: 60; onTriggered: root.refresh() }

  Component.onCompleted: refresh()

  ShellIpc {
    target: "backline.record"
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): void { root.refresh() }
    function state(): string {
      return JSON.stringify({ opened: root.opened, selectedIndex: root.selectedIndex, current: root.current,
        recorder: root.model.state, armed: root.model.armed, sources: root.sources.map(function(s) { return s.name }),
        takes: root.takes.map(function(t) { return t.take }), soundcheck: root.model.soundcheck, inputs: root.model.inputs,
        seconds: root.recordedSeconds })
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
    text: root.recording ? "󰑋" : "󰑊"
    active: root.recording
    labelText: root.soundchecking && !root.recording ? "󰐊" : ""
    tooltipText: root.t("record.title", "Record")
    onPressed: function(b) {
      if (b === Qt.RightButton) root.run(["omarchy-backline-record", "toggle"])
      else root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(400))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight, Style.space(620))

    content: PanelKeyCatcher {
      id: keyCatcher
      property alias scrollArea: scrollArea
      implicitHeight: column.implicitHeight
      anchors.fill: parent
      onMoveRequested: function(dx, dy) {
        if (dy !== 0) root.moveCursor(dy)
        else root.sideways(dx)
      }
      onActivateRequested: root.activate()
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      onTextKey: function(t) { root.textKey(t) }

      ScrollView {
        id: scrollArea
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Column {
          id: column
          width: scrollArea.availableWidth
          spacing: Style.space(12)

          // ---- Hero: state, and the record button ----
          Item {
            width: parent.width
            implicitHeight: Math.max(heroIcon.implicitHeight, heroLabels.implicitHeight)

            Text {
              id: heroIcon
              textFormat: Text.PlainText
              text: root.recording ? "󰑋" : "󰑊"
              color: root.recording ? Commons.Color.urgent : root.bar.foreground
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.display
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
            }

            Column {
              id: heroLabels
              anchors.left: heroIcon.right
              anchors.leftMargin: Style.space(14)
              anchors.right: recordButton.left
              anchors.rightMargin: Style.space(8)
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(2)

              Text {
                textFormat: Text.PlainText
                text: root.t("record.title", "Record")
                color: root.bar.foreground
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.title
                font.bold: true
              }

              Text {
                textFormat: Text.PlainText
                text: !root.model.running && root.model.state === "idle" && (root.model.armed || []).length === 0
                  ? root.t("record.idle", "READY")
                  : Model.recordHero(root.model, root.recordedSeconds, root.t)
                color: root.recording ? Commons.Color.urgent : Qt.darker(root.bar.foreground, 1.4)
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                font.letterSpacing: 1.2
                width: parent.width
                elide: Text.ElideRight
              }
            }

            Button {
              id: recordButton
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              iconText: root.recording ? "󰓛" : "󰑊"
              text: root.recording ? root.t("record.stop", "Stop") : root.t("record.record", "Record")
              active: root.recording
              accent: Commons.Color.urgent
              foreground: root.recording ? Commons.Color.urgent : root.bar.foreground
              fontFamily: root.bar.fontFamily
              tooltipText: root.t("record.recordHint", "Record or stop (r)")
              onClicked: root.run(["omarchy-backline-record", "toggle"])
            }
          }

          PanelSeparator { foreground: root.bar.foreground }

          // ---- Sources ----
          Column {
            width: parent.width
            spacing: Style.space(4)

            PanelSectionHeader {
              text: root.t("record.sources", "SOURCES")
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
            }

            Repeater {
              model: root.sources
              SourceRow {
                required property var modelData
                required property int index
                width: column.width
                row: modelData
                rowIndex: index
              }
            }

            Text {
              visible: root.sources.length === 0
              textFormat: Text.PlainText
              text: root.t("record.noSources", "No audio sources")
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.body
              leftPadding: Style.space(6)
            }
          }

          // ---- Soundcheck ----
          PanelSeparator { visible: root.soundchecking; foreground: root.bar.foreground }

          Column {
            visible: root.soundchecking
            width: parent.width
            spacing: Style.space(4)

            PanelSectionHeader {
              text: root.t("record.soundcheck", "SOUNDCHECK")
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
            }

            Text {
              textFormat: Text.PlainText
              text: root.model.soundcheck || ""
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption
              leftPadding: Style.space(6)
            }

            Repeater {
              model: root.inputs
              InputRow {
                required property var modelData
                required property int index
                width: column.width
                row: modelData
                rowIndex: index
              }
            }

            CursorSurface {
              id: stopRow
              width: parent.width
              implicitHeight: stopLabel.implicitHeight + Style.spacing.lg
              hasCursor: root.isCurrent("stop", 0)
              foreground: root.bar.foreground
              Text {
                id: stopLabel
                textFormat: Text.PlainText
                text: "󰓛  " + root.t("record.stopSoundcheck", "Stop soundcheck (p)")
                color: root.bar.foreground
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.body
                anchors.left: parent.left
                anchors.leftMargin: Style.space(6)
                anchors.verticalCenter: parent.verticalCenter
              }
              MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onContainsMouseChanged: if (containsMouse) root.select("stop", 0)
                onClicked: root.run(["omarchy-backline-soundcheck", "stop"])
              }
            }
          }

          // ---- Takes ----
          PanelSeparator { visible: root.takes.length > 0; foreground: root.bar.foreground }

          Column {
            visible: root.takes.length > 0
            width: parent.width
            spacing: Style.space(4)

            PanelSectionHeader {
              text: root.t("record.takes", "TAKES")
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
            }

            Repeater {
              model: root.takes
              TakeRow {
                required property var modelData
                required property int index
                width: column.width
                row: modelData
                rowIndex: index
              }
            }
          }

          Text {
            visible: !root.model.running && root.model.state === "idle"
            textFormat: Text.PlainText
            text: root.t("record.keys", "Space arms · r records · m marks · p soundchecks")
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

  function ensureVisible(item) {
    var scroll = panel.loadedContent ? panel.loadedContent.scrollArea : null
    if (!item || !scroll) return
    var flick = scroll.contentItem
    if (!flick || flick.contentY === undefined) return
    var top = item.mapToItem(flick.contentItem || flick, 0, 0).y
    var bottom = top + item.height
    if (top < flick.contentY) flick.contentY = Math.max(0, top - 6)
    else if (bottom > flick.contentY + flick.height) flick.contentY = bottom + 6 - flick.height
  }

  // A source: armed state, name, kind, and its level.
  component SourceRow: CursorSurface {
    id: sourceRow
    required property var row
    required property int rowIndex
    readonly property bool armed: root.isArmed(row.name)
    readonly property var node: root.opened ? root.nodeFor(row) : null
    readonly property bool meterable: !!node && !!node.audio && !!node.audio.channels && node.audio.channels.length > 0

    hasCursor: root.isCurrent("source", rowIndex)
    foreground: root.bar.foreground
    implicitHeight: sourceColumn.implicitHeight + Style.spacing.lg
    onHasCursorChanged: if (hasCursor) root.ensureVisible(sourceRow)

    PwObjectTracker { objects: sourceRow.node ? [sourceRow.node] : [] }
    PwNodePeakMonitor {
      id: peak
      node: sourceRow.meterable ? sourceRow.node : null
      enabled: sourceRow.meterable && root.opened
    }

    Row {
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(6)
      anchors.rightMargin: Style.space(6)
      spacing: Style.space(8)

      // Armed is the urgent ring, recording fills it: the record semantics
      // every Backline view shares.
      Text {
        id: armGlyph
        textFormat: Text.PlainText
        text: sourceRow.armed ? (root.recording ? "󰑋" : "󰻃") : "󰝦"
        color: sourceRow.armed ? Commons.Color.urgent : Qt.darker(root.bar.foreground, 1.6)
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.title
        width: Style.space(22)
        horizontalAlignment: Text.AlignHCenter
        anchors.verticalCenter: parent.verticalCenter
      }

      Column {
        id: sourceColumn
        width: parent.width - armGlyph.width - Style.space(8)
        spacing: Style.space(3)

        Item {
          width: parent.width
          implicitHeight: sourceName.implicitHeight
          Text {
            id: sourceName
            textFormat: Text.PlainText
            text: sourceRow.row.label
            color: root.bar.foreground
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
            font.bold: sourceRow.armed
            elide: Text.ElideRight
            anchors.left: parent.left
            anchors.right: kindText.left
            anchors.rightMargin: Style.space(8)
          }
          Text {
            id: kindText
            textFormat: Text.PlainText
            text: sourceRow.row.kind === "input" ? root.t("record.kindInput", "INPUT")
              : (sourceRow.row.kind === "instrument" ? root.t("record.kindInstrument", "INSTRUMENT") : root.t("record.kindOutput", "OUTPUT"))
            color: Qt.darker(root.bar.foreground, 1.6)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
            font.bold: true
            anchors.right: parent.right
            anchors.verticalCenter: sourceName.verticalCenter
          }
        }

        Rectangle {
          visible: sourceRow.meterable
          width: parent.width
          height: Math.max(Style.space(4), Style.spacing.xs)
          color: Util.alpha(root.bar.foreground, 0.14)
          Rectangle {
            height: parent.height
            width: parent.width * Math.max(0, Math.min(1, peak.peak))
            color: peak.peak >= 0.999 ? Commons.Color.urgent : root.bar.foreground
          }
        }
      }
    }

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onContainsMouseChanged: if (containsMouse) root.select("source", sourceRow.rowIndex)
      onClicked: { root.select("source", sourceRow.rowIndex); root.activate() }
    }
  }

  // A soundcheck input with its LIVE and PLAYBACK pills.
  component InputRow: CursorSurface {
    id: inputRow
    required property var row
    required property int rowIndex
    hasCursor: root.isCurrent("input", rowIndex)
    foreground: root.bar.foreground
    implicitHeight: Math.max(inputName.implicitHeight, pills.implicitHeight) + Style.spacing.md
    onHasCursorChanged: if (hasCursor) root.ensureVisible(inputRow)

    Text {
      id: inputName
      textFormat: Text.PlainText
      text: inputRow.row.node
      color: root.bar.foreground
      font.family: root.bar.fontFamily
      font.pixelSize: Style.font.body
      elide: Text.ElideRight
      anchors.left: parent.left
      anchors.leftMargin: Style.space(6)
      anchors.right: pills.left
      anchors.rightMargin: Style.space(8)
      anchors.verticalCenter: parent.verticalCenter
    }

    Row {
      id: pills
      anchors.right: parent.right
      anchors.rightMargin: Style.space(6)
      anchors.verticalCenter: parent.verticalCenter
      spacing: Style.space(4)
      Button {
        text: root.t("record.live", "LIVE")
        selected: inputRow.row.mode === "live"
        hasCursor: inputRow.hasCursor && inputRow.row.mode !== "live"
        foreground: root.bar.foreground
        fontFamily: root.bar.fontFamily
        fontSize: Style.font.caption
        onClicked: root.setInput(inputRow.row.node, "live")
      }
      Button {
        text: root.t("record.playback", "PLAYBACK")
        selected: inputRow.row.mode === "playback"
        hasCursor: inputRow.hasCursor && inputRow.row.mode !== "playback"
        accent: Commons.Color.accent
        foreground: root.bar.foreground
        fontFamily: root.bar.fontFamily
        fontSize: Style.font.caption
        onClicked: root.setInput(inputRow.row.node, "playback")
      }
    }

    HoverHandler { onHoveredChanged: if (hovered) root.select("input", inputRow.rowIndex) }
  }

  // A finished take: name, length, markers; Enter plays it for soundcheck.
  component TakeRow: CursorSurface {
    id: takeRow
    required property var row
    required property int rowIndex
    readonly property bool playing: root.model.soundcheck === row.take
    hasCursor: root.isCurrent("take", rowIndex)
    current: playing
    foreground: root.bar.foreground
    implicitHeight: takeName.implicitHeight + Style.spacing.lg
    onHasCursorChanged: if (hasCursor) root.ensureVisible(takeRow)

    Row {
      anchors.left: parent.left
      anchors.leftMargin: Style.space(6)
      anchors.right: parent.right
      anchors.rightMargin: Style.space(6)
      anchors.verticalCenter: parent.verticalCenter
      spacing: Style.space(8)

      Text {
        textFormat: Text.PlainText
        text: takeRow.playing ? "󰐊" : "󰎆"
        color: root.bar.foreground
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.body
        width: Style.space(22)
        horizontalAlignment: Text.AlignHCenter
      }
      Text {
        id: takeName
        textFormat: Text.PlainText
        text: takeRow.row.take
        color: root.bar.foreground
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.body
        font.bold: takeRow.playing
        elide: Text.ElideRight
        width: parent.width - Style.space(22) - takeMeta.width - Style.space(16)
      }
      Text {
        id: takeMeta
        textFormat: Text.PlainText
        text: (takeRow.row.markers > 0 ? root.t("record.takeMetaMarkers", "%1 · %2 TR · %3 M") : root.t("record.takeMeta", "%1 · %2 TR"))
          .arg(Model.shortLength(takeRow.row.seconds)).arg((takeRow.row.tracks || []).length).arg(takeRow.row.markers)
        color: Qt.darker(root.bar.foreground, 1.4)
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
        font.bold: true
      }
    }

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onContainsMouseChanged: if (containsMouse) root.select("take", takeRow.rowIndex)
      onClicked: { root.select("take", takeRow.rowIndex); root.activate() }
    }
  }
}
