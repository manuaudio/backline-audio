import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import Quickshell.Services.Pipewire
import qs.Ui
import qs.Commons
import qs.Commons as Commons
import "Model.js" as Model

// Instruments: the system instruments Backline's host is running, the MIDI
// input each one follows, and the picker that loads one from the plugin
// registry. Every instrument is its own process and PipeWire node, so a
// crash shows here as RESTARTING and never takes the panel or another
// instrument down. State comes from omarchy-backline-state on open and on
// every host signal while open (omarchy-backline-watch); closed, the panel
// only re-reads when a Backline node appears or goes away in PipeWire.
Panel {
  id: root
  moduleName: "backline.instruments"
  ipcTarget: "backline.instruments"
  manageIpc: false

  property QtObject strings: Commons.Translations { directory: Qt.resolvedUrl("i18n") }
  function t(key, english) { return root.strings.t(key, english) }

  property var model: ({ running: false, instances: [], midi: [], choices: [], last: "" })
  property bool stateStale: false
  property string busyText: ""

  // ---- Cursor: one model for keyboard and mouse ----
  // mode "list": rows are the instances, then the Add row.
  // mode "picker": rows are the filtered registry instruments.
  property string mode: "list"
  property int selectedIndex: 0
  property int pillIndex: -1
  property string filterText: ""

  readonly property var instances: model.instances || []
  readonly property var midiInputs: model.midi || []
  readonly property var pickerRows: Model.filterChoices(model.choices, filterText, model.last)
  readonly property int addIndex: instances.length
  readonly property var currentInstance: mode === "list" && selectedIndex < instances.length ? instances[selectedIndex] : null

  function rowCount() { return mode === "picker" ? pickerRows.length : instances.length + 1 }

  function moveCursor(dy) {
    var next = Math.max(0, Math.min(rowCount() - 1, selectedIndex + dy))
    if (next !== selectedIndex) {
      selectedIndex = next
      resetPill()
    }
  }

  // The MIDI pill under the cursor starts on the input the instrument follows.
  function resetPill() {
    pillIndex = currentInstance ? Model.midiIndex(midiInputs, currentInstance.midi) : -1
  }

  function movePill(dx) {
    if (!currentInstance || midiInputs.length === 0) return
    var from = pillIndex < 0 ? 0 : pillIndex
    pillIndex = Math.max(0, Math.min(midiInputs.length - 1, from + dx))
  }

  function activate(modifiers) {
    if (mode === "picker") {
      var row = pickerRows[selectedIndex]
      if (!row) return
      // A failing plugin is refused unless asked for with Shift+Enter (the
      // host's force), which the row says.
      var force = row.grade === "fail" && (modifiers & Qt.ShiftModifier)
      if (row.grade === "fail" && !force) return
      run(["omarchy-backline-instrument-add", row.path].concat(force ? ["--force"] : []), row.name)
      leavePicker()
      return
    }
    if (selectedIndex === addIndex) { enterPicker(); return }
    var inst = currentInstance
    var input = midiInputs[pillIndex]
    if (inst && input && input.match !== inst.midi)
      run(["omarchy-backline-instrument-midi", inst.id, input.match], inst.name)
  }

  function removeCurrent() {
    if (mode !== "list" || !currentInstance) return
    run(["omarchy-backline-instrument-remove", currentInstance.id], "")
  }

  function enterPicker() {
    mode = "picker"
    filterText = ""
    selectedIndex = 0
    Qt.callLater(function() { if (panel.loadedContent) panel.loadedContent.filter.forceActiveFocus() })
  }

  function leavePicker() {
    mode = "list"
    filterText = ""
    selectedIndex = instances.length === 0 ? addIndex : Math.min(selectedIndex, addIndex)
    if (panel.loadedContent) panel.loadedContent.forceActiveFocus()
  }

  // ---- State ----
  function refresh() {
    if (stateProc.running) { stateStale = true; return }
    stateProc.running = true
  }

  function applyState(text) {
    var next = Model.parseJson(text, null)
    if (!next) return
    model = next
    if (mode === "list") {
      if (selectedIndex > addIndex) selectedIndex = addIndex
      if (busyText === "" || !actionProc.running) resetPill()
    }
  }

  function run(command, label) {
    if (actionProc.running) return
    busyText = label
    actionProc.command = command
    actionProc.running = true
  }

  function openMixer() { Quickshell.execDetached(["omarchy-backline-mixer"]) }

  onOpenedChanged: {
    if (opened) {
      mode = "list"
      filterText = ""
      // With nothing loaded the cursor waits on Add, so Enter picks one.
      selectedIndex = instances.length === 0 ? 0 : 0
      refresh()
    }
  }

  // Closed, a Backline instrument node appearing or going away (a load, a
  // crash, a restart) is the cue to re-read for the bar icon.
  readonly property string instrumentNodes: {
    var names = []
    var nodes = Pipewire.nodes ? Pipewire.nodes.values : []
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i]
      if (n && String(n.name || "").indexOf("backline.") === 0 && n.isStream) names.push(String(n.name))
    }
    return names.sort().join(" ")
  }
  onInstrumentNodesChanged: refreshTimer.restart()

  // The level of each running instrument, while the panel is open. Rows hold
  // names only; the node is resolved here, never kept in a delegate.
  function nodeNamed(name) {
    var nodes = Pipewire.nodes ? Pipewire.nodes.values : []
    for (var i = 0; i < nodes.length; i++)
      if (nodes[i] && String(nodes[i].name || "") === name) return nodes[i]
    return null
  }

  Process {
    id: stateProc
    command: ["omarchy-backline-state", "instruments"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.applyState(text)
    }
    onExited: if (root.stateStale) { root.stateStale = false; Qt.callLater(root.refresh) }
  }

  Process {
    id: actionProc
    onExited: { root.busyText = ""; root.refresh() }
  }

  Process {
    id: watchProc
    running: root.opened
    command: ["omarchy-backline-watch"]
    stdout: SplitParser {
      onRead: function(line) { if (line.indexOf('"service":"host"') >= 0) refreshTimer.restart() }
    }
  }

  Timer {
    id: refreshTimer
    interval: 80
    onTriggered: root.refresh()
  }

  Component.onCompleted: refresh()

  ShellIpc {
    target: "backline.instruments"
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): void { root.refresh() }
    // For tests and scripts: what the panel shows and where its cursor is.
    function state(): string {
      return JSON.stringify({ opened: root.opened, mode: root.mode, selectedIndex: root.selectedIndex,
        pillIndex: root.pillIndex, filter: root.filterText, instances: root.instances,
        picker: root.pickerRows.map(function(r) { return r.name }), busy: root.busyText })
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
    text: "󰌌"
    labelText: root.instances.length > 0 ? Commons.I18n.formatNumber(root.instances.length) : ""
    active: root.instances.some(function(i) { return i.state === "failed" })
    tooltipText: root.t("instruments.title", "Instruments")
    onPressed: function(b) { if (b === Qt.RightButton) root.openMixer(); else root.toggle() }
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
      property alias filter: filterInput
      property alias scrollArea: scrollArea
      implicitHeight: column.implicitHeight
      anchors.fill: parent
      blocked: root.mode === "picker"
      onMoveRequested: function(dx, dy) {
        if (dy !== 0) root.moveCursor(dy)
        else root.movePill(dx)
      }
      onActivateRequested: function(modifiers) { root.activate(modifiers) }
      onDeleteRequested: root.removeCurrent()
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      onTextKey: function(t) {
        if (t === "o") root.openMixer()
        else if (t === "a" || t === "+") root.enterPicker()
      }

      // In the picker the filter line types; this sees its keys first and
      // keeps the arrows, Enter, Esc and Tab.
      Item {
        id: pickerKeys
        Keys.onPressed: function(event) {
          var ctrl = event.modifiers & Qt.ControlModifier
          if (event.key === Qt.Key_Escape) { root.leavePicker(); event.accepted = true }
          else if (event.key === Qt.Key_Down || (ctrl && event.key === Qt.Key_J)) { root.moveCursor(1); event.accepted = true }
          else if (event.key === Qt.Key_Up || (ctrl && event.key === Qt.Key_K)) { root.moveCursor(-1); event.accepted = true }
          else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { root.activate(event.modifiers); event.accepted = true }
          else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) { event.accepted = true }
        }
      }

      ScrollView {
        id: scrollArea
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Column {
          id: column
          width: scrollArea.availableWidth
          spacing: Style.space(12)

          // ---- Hero ----
          Item {
            width: parent.width
            implicitHeight: Math.max(heroIcon.implicitHeight, heroLabels.implicitHeight)

            Text {
              id: heroIcon
              textFormat: Text.PlainText
              text: "󰌌"
              color: root.bar.foreground
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.display
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
            }

            Column {
              id: heroLabels
              anchors.left: heroIcon.right
              anchors.leftMargin: Style.space(14)
              anchors.right: mixerButton.left
              anchors.rightMargin: Style.space(8)
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(2)

              Text {
                textFormat: Text.PlainText
                text: root.t("instruments.title", "Instruments")
                color: root.bar.foreground
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.title
                font.bold: true
                width: parent.width
                elide: Text.ElideRight
              }

              Text {
                textFormat: Text.PlainText
                text: root.busyText !== "" ? root.t("instruments.busy", "STARTING %1").arg(root.busyText.toUpperCase())
                  : Model.instrumentsHero(root.model, root.t)
                color: root.instances.some(function(i) { return i.state === "failed" }) ? Commons.Color.urgent : Qt.darker(root.bar.foreground, 1.4)
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                font.letterSpacing: 1.2
                width: parent.width
                elide: Text.ElideRight
              }
            }

            PanelActionButton {
              id: mixerButton
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              iconText: "󰕾"
              tooltipText: root.t("common.openMixer", "Open mixer (o)")
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
              onClicked: root.openMixer()
            }
          }

          PanelSeparator { foreground: root.bar.foreground }

          // ---- Running instruments, then Add ----
          Column {
            visible: root.mode === "list"
            width: parent.width
            spacing: Style.space(6)

            PanelSectionHeader {
              visible: root.instances.length > 0
              text: root.t("instruments.section", "PLAYING")
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
            }

            Repeater {
              model: root.instances
              InstanceRow {
                required property var modelData
                required property int index
                width: column.width
                row: modelData
                rowIndex: index
              }
            }

            CursorSurface {
              id: addRow
              width: parent.width
              implicitHeight: addLabel.implicitHeight + Style.spacing.xl
              hasCursor: root.selectedIndex === root.addIndex
              foreground: root.bar.foreground
              onHasCursorChanged: if (hasCursor) root.ensureVisible(addRow)

              Row {
                anchors.left: parent.left
                anchors.leftMargin: Style.space(6)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Style.space(8)
                Text {
                  textFormat: Text.PlainText
                  text: "󰐕"
                  color: root.bar.foreground
                  font.family: root.bar.fontFamily
                  font.pixelSize: Style.font.title
                  width: Style.space(22)
                  horizontalAlignment: Text.AlignHCenter
                }
                Text {
                  id: addLabel
                  textFormat: Text.PlainText
                  text: root.t("instruments.add", "Add instrument")
                  color: root.bar.foreground
                  font.family: root.bar.fontFamily
                  font.pixelSize: Style.font.body
                }
              }

              MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onContainsMouseChanged: if (containsMouse) root.selectedIndex = root.addIndex
                onClicked: root.enterPicker()
              }
            }
          }

          // ---- Picker ----
          Column {
            visible: root.mode === "picker"
            width: parent.width
            spacing: Style.space(6)

            Item {
              width: parent.width
              implicitHeight: filterInput.implicitHeight + Style.spacing.lg

              Text {
                textFormat: Text.PlainText
                visible: filterInput.text === ""
                text: root.t("instruments.filter", "Type to find an instrument")
                color: Qt.darker(root.bar.foreground, 1.6)
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.body
                anchors.left: parent.left
                anchors.leftMargin: Style.space(6)
                anchors.verticalCenter: parent.verticalCenter
              }

              FilterInput {
                id: filterInput
                keyHandler: pickerKeys
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Style.space(6)
                anchors.verticalCenter: parent.verticalCenter
                color: root.bar.foreground
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.body
                text: root.filterText
                onTextChanged: { root.filterText = text; root.selectedIndex = 0 }
              }
            }

            PanelSeparator { foreground: root.bar.foreground }

            Repeater {
              model: root.pickerRows
              ChoiceRow {
                required property var modelData
                required property int index
                width: column.width
                row: modelData
                rowIndex: index
              }
            }

            Text {
              visible: root.pickerRows.length === 0
              textFormat: Text.PlainText
              text: root.t("instruments.noMatch", "No instrument matches")
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.body
              leftPadding: Style.space(6)
            }
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

  // A running instrument: name, status, a level meter, and (under the
  // cursor) its MIDI input pills.
  component InstanceRow: CursorSurface {
    id: instanceRow
    required property var row
    required property int rowIndex
    readonly property bool cursorHere: root.mode === "list" && root.selectedIndex === rowIndex
    readonly property var node: root.opened ? root.nodeNamed(row.node) : null
    // Quickshell's peak monitor needs the node's channel layout; a node that
    // doesn't publish one gets no meter rather than a wrong one.
    readonly property bool meterable: !!node && !!node.audio && !!node.audio.channels && node.audio.channels.length > 0

    hasCursor: cursorHere
    foreground: root.bar.foreground
    implicitHeight: rowColumn.implicitHeight + Style.spacing.xl
    onHasCursorChanged: if (hasCursor) root.ensureVisible(instanceRow)

    PwObjectTracker { objects: instanceRow.node ? [instanceRow.node] : [] }
    PwNodePeakMonitor {
      id: peak
      node: instanceRow.meterable ? instanceRow.node : null
      enabled: instanceRow.meterable && root.opened
    }

    Column {
      id: rowColumn
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(6)
      anchors.rightMargin: Style.space(6)
      spacing: Style.space(5)

      Item {
        width: parent.width
        implicitHeight: nameText.implicitHeight

        Text {
          id: nameText
          textFormat: Text.PlainText
          text: instanceRow.row.name
          color: root.bar.foreground
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.body
          font.bold: true
          elide: Text.ElideRight
          anchors.left: parent.left
          anchors.right: statusText.left
          anchors.rightMargin: Style.space(8)
        }

        Text {
          id: statusText
          textFormat: Text.PlainText
          text: Model.instrumentStatus(instanceRow.row, root.t)
          readonly property string tone: Model.statusTone(instanceRow.row.state)
          color: tone === "urgent" ? Commons.Color.urgent : (tone === "warn" ? Commons.Color.accent : Qt.darker(root.bar.foreground, 1.4))
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
          font.bold: true
          font.letterSpacing: 1.0
          anchors.right: parent.right
          anchors.verticalCenter: nameText.verticalCenter
        }
      }

      // Level: the node's peak, drawn as one bar; the urgent colour at 0 dBFS.
      Rectangle {
        visible: instanceRow.meterable
        width: parent.width
        height: Math.max(Style.space(4), Style.spacing.xs)
        color: Util.alpha(root.bar.foreground, 0.14)
        Rectangle {
          height: parent.height
          width: parent.width * Math.max(0, Math.min(1, peak.peak))
          color: peak.peak >= 0.999 ? Commons.Color.urgent : root.bar.foreground
        }
      }

      Text {
        visible: !instanceRow.cursorHere
        textFormat: Text.PlainText
        text: Model.midiLabel(root.midiInputs[Model.midiIndex(root.midiInputs, instanceRow.row.midi)], root.t)
        color: Qt.darker(root.bar.foreground, 1.4)
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
        elide: Text.ElideRight
        width: parent.width
      }

      // MIDI pills: h/l moves along them, Enter applies; the one the
      // instrument follows now is the selected pill.
      Flow {
        visible: instanceRow.cursorHere
        width: parent.width
        spacing: Style.space(4)
        Repeater {
          model: root.midiInputs
          Button {
            required property var modelData
            required property int index
            text: Model.midiLabel(modelData, root.t)
            selected: modelData.match === instanceRow.row.midi
            hasCursor: index === root.pillIndex
            foreground: root.bar.foreground
            fontFamily: root.bar.fontFamily
            fontSize: Style.font.caption
            onHovered: function(on) { if (on) root.pillIndex = index }
            onClicked: { root.pillIndex = index; root.activate(0) }
          }
        }
      }

      Text {
        visible: instanceRow.row.failReason !== ""
        textFormat: Text.PlainText
        text: instanceRow.row.failReason
        color: Commons.Color.urgent
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
        wrapMode: Text.Wrap
        width: parent.width
      }
    }

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      acceptedButtons: Qt.NoButton
      onContainsMouseChanged: if (containsMouse && root.selectedIndex !== instanceRow.rowIndex) {
        root.selectedIndex = instanceRow.rowIndex
        root.resetPill()
      }
    }
  }

  // One registry instrument in the picker, with its grade.
  component ChoiceRow: CursorSurface {
    id: choiceRow
    required property var row
    required property int rowIndex
    hasCursor: root.mode === "picker" && root.selectedIndex === rowIndex
    foreground: root.bar.foreground
    implicitHeight: choiceColumn.implicitHeight + Style.spacing.lg
    onHasCursorChanged: if (hasCursor) root.ensureVisible(choiceRow)
    opacity: row.grade === "fail" ? 0.6 : 1.0

    Column {
      id: choiceColumn
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(6)
      anchors.rightMargin: Style.space(6)
      spacing: Style.space(2)

      Item {
        width: parent.width
        implicitHeight: choiceName.implicitHeight
        Text {
          id: choiceName
          textFormat: Text.PlainText
          text: choiceRow.row.name
          color: root.bar.foreground
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.body
          font.bold: choiceRow.row.path === root.model.last
          elide: Text.ElideRight
          anchors.left: parent.left
          anchors.right: gradeText.left
          anchors.rightMargin: Style.space(8)
        }
        Text {
          id: gradeText
          textFormat: Text.PlainText
          text: Model.gradeLabel(choiceRow.row.grade, root.t)
          color: choiceRow.row.grade === "fail" ? Commons.Color.urgent : (choiceRow.row.grade === "warn" ? Commons.Color.accent : Qt.darker(root.bar.foreground, 1.4))
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
          font.bold: true
          anchors.right: parent.right
          anchors.verticalCenter: choiceName.verticalCenter
        }
      }

      Text {
        textFormat: Text.PlainText
        text: choiceRow.row.grade === "fail" && choiceRow.hasCursor
          ? root.t("instruments.forceHint", "Fails validation · Shift+Enter loads it anyway")
          : choiceRow.row.vendor
        color: choiceRow.row.grade === "fail" && choiceRow.hasCursor ? Commons.Color.urgent : Qt.darker(root.bar.foreground, 1.4)
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
        elide: Text.ElideRight
        width: parent.width
      }
    }

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onContainsMouseChanged: if (containsMouse) root.selectedIndex = choiceRow.rowIndex
      onClicked: function(mouse) { root.selectedIndex = choiceRow.rowIndex; root.activate(mouse.modifiers) }
    }
  }
}
