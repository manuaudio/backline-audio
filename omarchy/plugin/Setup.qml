import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import Quickshell.Services.Pipewire
import qs.Ui
import qs.Commons
import qs.Commons as Commons
import "Model.js" as Model

// Audio Setup: what macOS keeps in Audio MIDI Setup and Omarchy's own audio
// panel leaves out. Pro audio (a small buffer and a locked sample rate, for
// recording), devices combined into aggregate or multi-output devices, and
// the MIDI ports that send, with where each sends. Volume, outputs and the app
// mixer stay in Omarchy's audio panel.
//
// None of this has a Quickshell service, so the commands report it:
// omarchy-backline-pro status --json, omarchy-backline-aggregate list --json
// and omarchy-backline-midi list --json, read when the panel opens and when a
// command pushes `backline.setup refresh` after a change. No timer. The pro
// audio status is also read once at startup, so the bar icon is true.
//
// Sample-rate conversion is shown only while it happens and only here, in
// muted text under the sample rate and on a combined device's row: "SRC 44.1 →
// 48 kHz". omarchy-backline-src reads it from pw-dump when the panel opens, and
// again when PipeWire's nodes change while it is open; closed, it reads nothing.
Panel {
  id: root
  moduleName: "backline.setup"
  ipcTarget: "backline.setup"
  manageIpc: false

  // The Backline bar widget that holds this panel and its siblings: Tab moves
  // to the sibling, and past the last one to the bar's next panel.
  property Item host: null
  function tabTo(direction) {
    if (host && typeof host.switchFrom === "function") return host.switchFrom(root, direction)
    return root.switchPanel(direction)
  }

  property QtObject strings: Commons.Translations { directory: Qt.resolvedUrl("i18n") }
  function t(key, english) { return root.strings.t(key, english) }

  // Null while PipeWire isn't answering, which hides the pro audio section.
  property var proStatus: null
  readonly property bool proOn: !!proStatus && proStatus.enabled
  property bool proStale: false
  // A change asked for while another runs waits here; only the latest counts.
  property var proQueued: null
  property var combined: []
  property var midiPorts: []
  // Streams PipeWire is converting to or from their device's rate, while open.
  property var conversions: []
  property bool srcStale: false
  readonly property var srcLines: Model.srcLines(conversions, root.t, Commons.I18n.formatNumber)
  readonly property var midiRows: Model.midiSources(midiPorts)

  // ---- Cursor ----
  readonly property var rows: Model.setupRows(proStatus, combined, midiRows)
  property int selectedIndex: 0
  readonly property var currentRow: rows[Math.max(0, Math.min(rows.length - 1, selectedIndex))] || { kind: "add" }
  // The pill the cursor sits on in each pill row.
  property int bufferPill: 0
  property int ratePill: 0
  property int addPill: 0

  function rowIndex(kind, index) {
    for (var i = 0; i < rows.length; i++)
      if (rows[i].kind === kind && (index === undefined || rows[i].index === index)) return i
    return -1
  }

  function cursorOn(kind, index) {
    return root.opened && root.currentRow.kind === kind && (index === undefined || root.currentRow.index === index)
  }

  function clampCursor() {
    if (selectedIndex > rows.length - 1) selectedIndex = Math.max(0, rows.length - 1)
  }

  function moveCursor(dy) {
    selectedIndex = Math.max(0, Math.min(rows.length - 1, selectedIndex + dy))
  }

  function sideways(dx) {
    var kind = currentRow.kind
    if (kind === "buffer") bufferPill = Math.max(0, Math.min(Model.PRO_BUFFERS.length - 1, bufferPill + dx))
    else if (kind === "rate") ratePill = Math.max(0, Math.min(Model.PRO_RATES.length - 1, ratePill + dx))
    else if (kind === "add") addPill = Math.max(0, Math.min(1, addPill + dx))
  }

  function activate() {
    var row = currentRow
    if (row.kind === "pro") setPro(!proOn)
    else if (row.kind === "buffer") runPro(["on", "--buffer", String(Model.PRO_BUFFERS[bufferPill])])
    else if (row.kind === "rate") runPro(["on", "--rate", String(Model.PRO_RATES[ratePill])])
    else if (row.kind === "add") addDevice(addPill === 1)
    else if (row.kind === "midi") connectMidi(midiRows[row.index])
  }

  function removeCurrent() {
    if (currentRow.kind === "device" && combined[currentRow.index])
      run(["omarchy-backline-aggregate", "remove", combined[currentRow.index].name])
  }

  // Parks the pill cursors on the values in force, except under a cursor
  // walking a row, which a refresh must not snap back.
  function syncPills() {
    if (!proStatus) return
    if (opened && (currentRow.kind === "buffer" || currentRow.kind === "rate")) return
    bufferPill = Model.pillIndex(Model.PRO_BUFFERS, proStatus.buffer)
    ratePill = Model.pillIndex(Model.PRO_RATES, proStatus.rate)
  }

  // ---- Commands ----
  function readPro() {
    if (proStatusProc.running) { proStale = true; return }
    proStatusProc.running = true
  }

  function readSrc() {
    if (!opened) return
    if (srcProc.running) { srcStale = true; return }
    srcProc.running = true
  }

  function refresh() {
    if (!proActionProc.running) readPro()
    if (!opened) return
    readSrc()
    if (!combinedProc.running) combinedProc.running = true
    if (!midiProc.running) midiProc.running = true
  }

  function runPro(args) {
    var command = ["omarchy-backline-pro"].concat(args)
    if (proActionProc.running) { proQueued = command; return }
    proActionProc.command = command
    proActionProc.running = true
  }

  function setPro(on) { runPro([on ? "on" : "off"]) }

  function run(command) {
    if (actionProc.running) return
    actionProc.command = command
    actionProc.running = true
  }

  // Making a device and choosing where MIDI goes are the menu's pickers, so
  // the panel steps aside for them.
  function addDevice(mirror) {
    close()
    Quickshell.execDetached(mirror ? ["omarchy-backline-aggregate", "create", "--mirror"] : ["omarchy-backline-aggregate", "create"])
  }

  function connectMidi(port) {
    if (!port) return
    close()
    Quickshell.execDetached(["omarchy-backline-midi", "connect", port.address])
  }

  onOpenedChanged: {
    if (opened) {
      selectedIndex = 0
      refresh()
    } else {
      conversions = []
    }
  }

  // A stream starting or stopping is when a conversion starts or stops; the
  // short wait lets the device settle on its rate first.
  Connections {
    target: Pipewire.nodes
    function onValuesChanged() { if (root.opened) srcTimer.restart() }
  }

  Timer { id: srcTimer; interval: 300; onTriggered: root.readSrc() }

  Component.onCompleted: readPro()

  Process {
    id: proStatusProc
    command: ["omarchy-backline-pro", "status", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        root.proStatus = Model.parseProStatus(text)
        root.syncPills()
        // Switching off takes the pill rows away from under the cursor.
        root.clampCursor()
      }
    }
    onExited: if (root.proStale) { root.proStale = false; Qt.callLater(root.readPro) }
  }

  Process {
    id: proActionProc
    onExited: {
      root.readPro()
      // A locked rate changes what has to be converted.
      srcTimer.restart()
      if (!root.proQueued) return
      var command = root.proQueued
      root.proQueued = null
      Qt.callLater(function() { root.runPro(command.slice(1)) })
    }
  }

  Process {
    id: srcProc
    command: ["omarchy-backline-src", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: if (root.opened) root.conversions = Model.parseConversions(text)
    }
    onExited: if (root.srcStale) { root.srcStale = false; Qt.callLater(root.readSrc) }
  }

  Process {
    id: combinedProc
    command: ["omarchy-backline-aggregate", "list", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: { root.combined = Model.parseCombined(text); root.clampCursor() }
    }
  }

  Process {
    id: midiProc
    command: ["omarchy-backline-midi", "list", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: { root.midiPorts = Model.parseMidiPorts(text); root.clampCursor() }
    }
  }

  Process {
    id: actionProc
    onExited: root.refresh()
  }

  ShellIpc {
    target: "backline.setup"
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): void { root.refresh() }
    function state(): string {
      return JSON.stringify({ opened: root.opened, row: root.currentRow.kind, index: root.currentRow.index,
        pro: root.proStatus, bufferPill: Model.PRO_BUFFERS[root.bufferPill], ratePill: Model.PRO_RATES[root.ratePill],
        combined: root.combined.map(function(d) { return d.name }),
        src: root.srcLines,
        midi: root.midiRows.map(function(p) { return p.address }) })
    }
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰝚"
    active: root.proOn
    tooltipText: root.t("setup.title", "Audio Setup")
    // Right click turns pro audio on or off, as the quick action.
    onPressed: function(b) { if (b === Qt.RightButton) root.setPro(!root.proOn); else root.toggle() }
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
      implicitHeight: column.implicitHeight
      anchors.fill: parent
      onMoveRequested: function(dx, dy) {
        if (dy !== 0) root.moveCursor(dy)
        else root.sideways(dx)
      }
      onActivateRequested: root.activate()
      onDeleteRequested: root.removeCurrent()
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.tabTo(direction) }
      onTextKey: function(t) {
        if (t === "p") root.setPro(!root.proOn)
        else if (t === "o") Quickshell.execDetached(["omarchy-backline-mixer"])
      }

      Column {
        id: column
        width: parent.width
        spacing: Style.space(12)

        // ---- Pro audio ----
        Column {
          visible: !!root.proStatus
          width: parent.width
          spacing: Style.space(10)

          // The switch rides on the header line, as the network panel's
          // Automatic band switch does, and the header says how long one
          // buffer takes while it is on.
          Item {
            width: parent.width
            implicitHeight: Math.max(proHeader.implicitHeight, proSwitch.implicitHeight)

            PanelSectionHeader {
              id: proHeader
              text: Model.proTitle(root.proStatus, root.t, Commons.I18n.formatNumber)
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
            }

            ToggleSwitch {
              id: proSwitch
              trackHeight: Math.round(proHeader.font.pixelSize * 1.2)
              cursorPad: Style.space(3)
              anchors.right: parent.right
              anchors.verticalCenter: proHeader.verticalCenter
              anchors.verticalCenterOffset: Math.round(proHeader.topPadding / 2)
              checked: root.proOn
              busy: proActionProc.running
              hasCursor: root.cursorOn("pro")
              foreground: root.bar.foreground
              onToggled: root.setPro(!root.proOn)
              onHovered: function(isHovered) { if (isHovered) root.selectedIndex = root.rowIndex("pro") }

              PanelToolTip {
                visible: proSwitch.containsMouse
                text: root.t("setup.proTooltip", "A small buffer and a locked sample rate, for recording (p)")
                fontFamily: root.bar.fontFamily
              }
            }
          }

          // Small buffers only hold with realtime scheduling.
          Text {
            visible: root.proOn && !root.proStatus.realtime
            width: parent.width
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: root.t("setup.notRealtime", "PipeWire isn't running realtime, so buffers under 256 frames may drop out.")
            color: Commons.Color.urgent
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
          }

          // Sample-rate conversion, only while it happens: quiet, for whoever
          // looks next to the sample rate.
          Repeater {
            model: root.srcLines

            Text {
              required property string modelData
              width: parent.width
              textFormat: Text.PlainText
              text: modelData
              elide: Text.ElideRight
              color: Qt.darker(root.bar.foreground, 1.6)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption

              HoverHandler { id: srcHover }
              PanelToolTip {
                visible: srcHover.hovered
                text: root.t("setup.srcTooltip", "PipeWire converts this audio to the device's sample rate")
                fontFamily: root.bar.fontFamily
              }
            }
          }

          // Collapses with the switch, the way the network panel's band pills do.
          Item {
            width: parent.width
            clip: true
            visible: height > 0
            height: root.proOn ? proPills.implicitHeight : 0
            opacity: root.proOn ? 1 : 0

            Behavior on height {
              NumberAnimation { duration: Style.duration(140); easing.type: Easing.OutCubic }
            }
            Behavior on opacity {
              NumberAnimation { duration: Style.duration(140); easing.type: Easing.OutCubic }
            }

            Column {
              id: proPills
              width: parent.width
              spacing: Style.space(6)

              // Both rows' pills start where the longer caption ends.
              readonly property real captionWidth: Math.max(bufferCaption.advanceWidth, rateCaption.advanceWidth) + Style.space(8)

              TextMetrics {
                id: bufferCaption
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                text: root.t("setup.buffer", "Buffer")
              }

              TextMetrics {
                id: rateCaption
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                text: root.t("setup.sampleRate", "Sample rate")
              }

              PillRow {
                kind: "buffer"
                caption: root.t("setup.buffer", "Buffer")
                values: Model.PRO_BUFFERS
                cursorIndex: root.bufferPill
                live: root.proStatus ? root.proStatus.buffer : 0
                captionWidth: proPills.captionWidth
              }

              PillRow {
                kind: "rate"
                caption: root.t("setup.sampleRate", "Sample rate")
                values: Model.PRO_RATES
                cursorIndex: root.ratePill
                live: root.proStatus ? root.proStatus.rate : 0
                captionWidth: proPills.captionWidth
              }
            }
          }
        }

        PanelSeparator {
          visible: !!root.proStatus
          foreground: root.bar.foreground
        }

        // ---- Combined devices ----
        Column {
          width: parent.width
          spacing: Style.space(6)

          PanelSectionHeader {
            text: root.t("setup.devices", "COMBINED DEVICES")
            foreground: root.bar.foreground
            fontFamily: root.bar.fontFamily
          }

          Repeater {
            model: root.combined

            ListRow {
              required property var modelData
              required property int index
              width: parent.width
              kind: "device"
              rowIndexInKind: index
              glyph: modelData.mirror ? "󰓃" : "󰴸"
              title: modelData.name
              caption: Model.combinedCaption(modelData, root.t, root.conversions, Commons.I18n.formatNumber)
              tip: root.t("setup.removeHint", "x removes it")
            }
          }

          CursorSurface {
            id: addRow
            width: parent.width
            implicitHeight: addPills.implicitHeight + Style.spacing.lg
            hasCursor: root.cursorOn("add")
            foreground: root.bar.foreground
            HoverHandler { onHoveredChanged: if (hovered) root.selectedIndex = root.rowIndex("add") }

            Row {
              id: addPills
              anchors.left: parent.left
              anchors.leftMargin: Style.space(6)
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(4)

              Repeater {
                model: [
                  { glyph: "󰴸", label: root.t("setup.addAggregate", "Aggregate Device"), tip: root.t("setup.addAggregateTip", "Several interfaces as one, channels stacked in order") },
                  { glyph: "󰓃", label: root.t("setup.addMultiOutput", "Multi-Output Device"), tip: root.t("setup.addMultiOutputTip", "The same sound on several outputs at once") }
                ]
                Button {
                  required property var modelData
                  required property int index
                  iconText: modelData.glyph
                  text: modelData.label
                  tooltipText: modelData.tip
                  hasCursor: root.cursorOn("add") && root.addPill === index
                  foreground: root.bar.foreground
                  fontFamily: root.bar.fontFamily
                  fontSize: Style.font.caption
                  onHovered: function(on) { if (on) { root.selectedIndex = root.rowIndex("add"); root.addPill = index } }
                  onClicked: root.addDevice(index === 1)
                }
              }
            }
          }
        }

        // ---- MIDI ----
        PanelSeparator {
          visible: root.midiRows.length > 0
          foreground: root.bar.foreground
        }

        Column {
          width: parent.width
          spacing: Style.space(6)
          visible: root.midiRows.length > 0

          PanelSectionHeader {
            text: root.t("setup.midi", "MIDI")
            foreground: root.bar.foreground
            fontFamily: root.bar.fontFamily
          }

          Repeater {
            model: root.midiRows

            ListRow {
              required property var modelData
              required property int index
              width: parent.width
              kind: "midi"
              rowIndexInKind: index
              glyph: "󰣱"
              title: Model.portLabel(modelData)
              caption: Model.portCaption(modelData, root.midiPorts, root.t)
              tip: root.t("setup.midiConnect", "Choose where it sends")
              onActivated: root.connectMidi(modelData)
            }
          }
        }

        Text {
          textFormat: Text.PlainText
          text: root.t("setup.keys", "p pro audio · h/l pills · Enter applies · x removes · o mixer")
          color: Qt.darker(root.bar.foreground, 1.6)
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
          width: parent.width
          wrapMode: Text.Wrap
        }
      }
    }
  }

  // A combined device or a MIDI port: a glyph, a name and a caption.
  component ListRow: CursorSurface {
    id: listRow
    required property string kind
    required property int rowIndexInKind
    required property string glyph
    required property string title
    required property string caption
    required property string tip
    signal activated()

    hasCursor: root.cursorOn(kind, rowIndexInKind)
    foreground: root.bar.foreground
    implicitHeight: inner.implicitHeight + Style.spacing.lg

    Row {
      id: inner
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.leftMargin: Style.space(6)
      anchors.rightMargin: Style.space(6)
      anchors.verticalCenter: parent.verticalCenter
      spacing: Style.space(8)

      Text {
        textFormat: Text.PlainText
        text: listRow.glyph
        color: root.bar.foreground
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.title
        width: Style.space(22)
        horizontalAlignment: Text.AlignHCenter
        anchors.verticalCenter: parent.verticalCenter
      }

      Column {
        width: parent.width - Style.space(22) - Style.space(8)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Style.space(2)

        Text {
          width: parent.width
          textFormat: Text.PlainText
          text: listRow.title
          color: root.bar.foreground
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.body
          elide: Text.ElideRight
        }

        Text {
          width: parent.width
          textFormat: Text.PlainText
          text: listRow.caption
          color: Qt.darker(root.bar.foreground, 1.4)
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
          elide: Text.ElideRight
        }
      }
    }

    MouseArea {
      id: rowMouse
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onContainsMouseChanged: if (containsMouse) root.selectedIndex = root.rowIndex(listRow.kind, listRow.rowIndexInKind)
      onClicked: listRow.activated()
    }

    PanelToolTip {
      visible: rowMouse.containsMouse
      text: listRow.tip
      fontFamily: root.bar.fontFamily
    }
  }

  // One row of pro audio pills: the buffer sizes or the sample rates. The
  // value in force is selected; the cursor walks the row with h/l.
  component PillRow: Row {
    id: pillRow
    required property string kind
    required property string caption
    required property var values
    required property int cursorIndex
    required property int live
    required property real captionWidth

    width: parent ? parent.width : 0
    spacing: Style.space(6)

    readonly property real cellWidth: (width - captionWidth - spacing * values.length) / Math.max(1, values.length)

    Text {
      width: pillRow.captionWidth
      anchors.verticalCenter: parent.verticalCenter
      textFormat: Text.PlainText
      text: pillRow.caption
      color: Qt.darker(root.bar.foreground, 1.4)
      font.family: root.bar.fontFamily
      font.pixelSize: Style.font.caption
      elide: Text.ElideRight
    }

    Repeater {
      model: pillRow.values

      delegate: Button {
        required property var modelData
        required property int index

        width: pillRow.cellWidth
        text: pillRow.kind === "buffer" ? Commons.I18n.formatNumber(modelData) : Model.rateLabel(modelData, Commons.I18n.formatNumber)
        tooltipText: pillRow.kind === "buffer"
          ? Model.bufferTooltip(modelData, root.proStatus ? root.proStatus.rate : 48000, root.t, Commons.I18n.formatNumber)
          : Model.rateTooltip(modelData, root.t, Commons.I18n.formatNumber)
        fontSize: Style.font.bodySmall
        foreground: root.bar.foreground
        fontFamily: root.bar.fontFamily
        verticalPadding: Style.spacing.controlPaddingY + Style.space(2)
        bordered: true
        selected: pillRow.live === modelData
        hasCursor: root.cursorOn(pillRow.kind) && pillRow.cursorIndex === index

        onClicked: {
          if (pillRow.kind === "buffer") root.runPro(["on", "--buffer", String(modelData)])
          else root.runPro(["on", "--rate", String(modelData)])
        }
        onHovered: function(isHovered) {
          if (!isHovered) return
          root.selectedIndex = root.rowIndex(pillRow.kind)
          if (pillRow.kind === "buffer") root.bufferPill = index
          else root.ratePill = index
        }
      }
    }
  }
}
