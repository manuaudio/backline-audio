import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons
import qs.Commons as Commons
import "Model.js" as Model

// Plugins: every installed CLAP, VST3 and LV2 plugin as Backline's registry
// graded it (pass, warn, fail), grouped by product across formats. The row
// under the cursor says why a plugin warns or fails, so checking one needs no
// Enter. The registry probes each binary in its own sandboxed process; this
// panel only reads its index (omarchy-backline-state plugins) on open, and
// again when a rescan changes it.
Panel {
  id: root
  moduleName: "backline.plugins"
  ipcTarget: "backline.plugins"
  manageIpc: false

  // The Backline bar widget (Backline.qml) that holds this panel and its
  // three siblings: Tab moves to the sibling, and past the last one to the
  // bar's next panel. Without a host (a bench strip), the bar's own switch.
  property Item host: null
  function tabTo(direction) {
    if (host && typeof host.switchFrom === "function") return host.switchFrom(root, direction)
    return root.switchPanel(direction)
  }

  property QtObject strings: Commons.Translations { directory: Qt.resolvedUrl("i18n") }
  function t(key, english) { return root.strings.t(key, english) }

  property var model: ({ total: 0, pass: 0, warn: 0, fail: 0, products: [] })
  property bool stateStale: false
  property bool scanning: false

  // Cursor: -1 is the pill row, 0.. the list.
  property int selectedIndex: 0
  property int pillIndex: 0
  property string filterText: ""
  property bool filtering: false
  property int detailIndex: -1

  readonly property string pill: Model.PLUGIN_PILLS[pillIndex]
  readonly property var rows: Model.filterProducts(model.products, filterText, pill)
  readonly property var currentRow: selectedIndex >= 0 ? rows[selectedIndex] || null : null

  function moveCursor(dy) {
    selectedIndex = Math.max(-1, Math.min(rows.length - 1, selectedIndex + dy))
    detailIndex = -1
    if (selectedIndex >= 0 && panel.loadedContent) panel.loadedContent.list.positionViewAtIndex(selectedIndex, ListView.Contain)
  }

  function movePill(dx) {
    pillIndex = Math.max(0, Math.min(Model.PLUGIN_PILLS.length - 1, pillIndex + dx))
    selectedIndex = rows.length > 0 ? Math.max(selectedIndex, -1) : -1
    detailIndex = -1
  }

  function activate() {
    if (selectedIndex < 0) return
    detailIndex = detailIndex === selectedIndex ? -1 : selectedIndex
    if (panel.loadedContent) panel.loadedContent.list.positionViewAtIndex(selectedIndex, ListView.Contain)
  }

  function startFilter() {
    filtering = true
    Qt.callLater(function() { if (panel.loadedContent) panel.loadedContent.filter.forceActiveFocus() })
  }

  function stopFilter(clear) {
    filtering = false
    if (clear) filterText = ""
    if (panel.loadedContent) panel.loadedContent.forceActiveFocus()
  }

  function addCurrent() {
    var row = currentRow
    if (!row || !row.hostable) return
    var clap = (row.formats || []).filter(function(f) { return f.format === "clap" })[0]
    if (clap) Quickshell.execDetached(["omarchy-backline-instrument-add", clap.path])
  }

  function rescan() {
    if (scanProc.running) return
    scanning = true
    scanProc.running = true
  }

  function refresh() {
    if (stateProc.running) { stateStale = true; return }
    stateProc.running = true
  }

  function applyState(text) {
    var next = Model.parseJson(text, null)
    if (!next) return
    model = next
    if (selectedIndex > rows.length - 1) selectedIndex = rows.length - 1
  }

  onFilterTextChanged: { selectedIndex = rows.length > 0 ? 0 : -1; detailIndex = -1 }

  onOpenedChanged: {
    if (opened) {
      filtering = false
      filterText = ""
      pillIndex = 0
      selectedIndex = 0
      detailIndex = -1
      refresh()
    }
  }

  Process {
    id: stateProc
    command: ["omarchy-backline-state", "plugins"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.applyState(text)
    }
    onExited: if (root.stateStale) { root.stateStale = false; Qt.callLater(root.refresh) }
  }

  Process {
    id: scanProc
    command: ["omarchy-backline-plugins-scan"]
    onExited: { root.scanning = false; root.refresh() }
  }

  ShellIpc {
    target: "backline.plugins"
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): void { root.refresh() }
    function state(): string {
      var row = root.currentRow
      return JSON.stringify({ opened: root.opened, selectedIndex: root.selectedIndex, pill: root.pill, filter: root.filterText,
        filtering: root.filtering, count: root.rows.length, total: root.model.total,
        current: row ? { name: row.name, grade: row.worst, finding: Model.findingLine(row) } : null,
        detail: root.detailIndex })
    }
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰏗"
    tooltipText: root.t("plugins.title", "Plugins")
    onPressed: function(b) { root.toggle() }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(440))
    contentHeight: panel.fittedContentHeight(Style.space(620), Style.space(620))

    content: PanelKeyCatcher {
      id: keyCatcher
      property alias filter: filterInput
      property alias list: list
      anchors.fill: parent
      implicitHeight: Style.space(620)
      blocked: root.filtering
      onMoveRequested: function(dx, dy) {
        if (dy !== 0) root.moveCursor(dy)
        else if (root.selectedIndex < 0) root.movePill(dx)
      }
      onActivateRequested: root.activate()
      onCloseRequested: {
        if (root.filterText !== "") root.filterText = ""
        else root.close()
      }
      onTabRequested: function(direction) { root.tabTo(direction) }
      onTextKey: function(t) {
        if (t === "/") root.startFilter()
        else if (t === "s") root.rescan()
        else if (t === "a") root.addCurrent()
        else if (t === "o") Quickshell.execDetached(["omarchy-backline-mixer"])
      }

      Item {
        id: filterKeys
        Keys.onPressed: function(event) {
          if (event.key === Qt.Key_Escape) { root.stopFilter(true); event.accepted = true }
          else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { root.stopFilter(false); event.accepted = true }
          else if (event.key === Qt.Key_Down) { root.stopFilter(false); root.moveCursor(1); event.accepted = true }
          else if (event.key === Qt.Key_Up) { root.moveCursor(-1); event.accepted = true }
          else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) { event.accepted = true }
        }
      }

      Column {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Style.space(10)

        // ---- Hero: the whole system's grades ----
        Item {
          width: parent.width
          implicitHeight: Math.max(heroIcon.implicitHeight, heroLabels.implicitHeight)

          Text {
            id: heroIcon
            textFormat: Text.PlainText
            text: "󰏗"
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
            anchors.right: scanButton.left
            anchors.rightMargin: Style.space(8)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            Text {
              textFormat: Text.PlainText
              text: root.t("plugins.title", "Plugins")
              color: root.bar.foreground
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.title
              font.bold: true
            }

            Row {
              spacing: Style.space(10)
              Text {
                textFormat: Text.PlainText
                text: root.scanning ? root.t("plugins.scanningHero", "SCANNING…")
                  : root.t("plugins.heroCount", "%1 INDEXED").arg(Commons.I18n.formatNumber(root.model.total))
                color: Qt.darker(root.bar.foreground, 1.4)
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                font.letterSpacing: 1.2
              }
              Text {
                textFormat: Text.PlainText
                text: root.t("plugins.heroPass", "%1 PASS").arg(Commons.I18n.formatNumber(root.model.pass))
                color: Qt.darker(root.bar.foreground, 1.4)
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                font.letterSpacing: 1.2
              }
              Text {
                textFormat: Text.PlainText
                text: root.t("plugins.heroWarn", "%1 WARN").arg(Commons.I18n.formatNumber(root.model.warn))
                color: Commons.Color.accent
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                font.letterSpacing: 1.2
              }
              Text {
                textFormat: Text.PlainText
                text: root.t("plugins.heroFail", "%1 FAIL").arg(Commons.I18n.formatNumber(root.model.fail))
                color: Commons.Color.urgent
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                font.letterSpacing: 1.2
              }
            }
          }

          PanelActionButton {
            id: scanButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            iconText: "󰑐"
            tooltipText: root.t("plugins.rescan", "Rescan (s)")
            foreground: root.bar.foreground
            fontFamily: root.bar.fontFamily
            onClicked: root.rescan()
          }
        }

        PanelSeparator { foreground: root.bar.foreground }

        // ---- Kind pills and the filter line ----
        Row {
          spacing: Style.space(4)
          Repeater {
            model: Model.PLUGIN_PILLS
            Button {
              required property var modelData
              required property int index
              text: modelData === "all" ? root.t("plugins.all", "All")
                : modelData === "instrument" ? root.t("plugins.instruments", "Instruments")
                : modelData === "effect" ? root.t("plugins.effects", "Effects")
                : root.t("plugins.issues", "Issues")
              selected: index === root.pillIndex
              hasCursor: root.selectedIndex < 0 && index === root.pillIndex
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
              fontSize: Style.font.caption
              onHovered: function(on) { if (on) root.selectedIndex = -1 }
              onClicked: { root.pillIndex = index; root.selectedIndex = 0 }
            }
          }
        }

        Item {
          width: parent.width
          implicitHeight: filterInput.implicitHeight + Style.spacing.md

          Text {
            textFormat: Text.PlainText
            visible: filterInput.text === ""
            text: root.filtering ? root.t("plugins.filterTyping", "Type a name or vendor")
              : root.t("plugins.filterHint", "/ filter · s rescan · a load as instrument")
            color: Qt.darker(root.bar.foreground, 1.6)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
            anchors.left: parent.left
            anchors.leftMargin: Style.space(6)
            anchors.verticalCenter: parent.verticalCenter
          }

          Text {
            textFormat: Text.PlainText
            visible: root.filtering || filterInput.text !== ""
            text: "/"
            color: Commons.Color.accent
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
            anchors.right: filterInput.left
            anchors.rightMargin: Style.space(4)
            anchors.verticalCenter: parent.verticalCenter
          }

          FilterInput {
            id: filterInput
            keyHandler: filterKeys
            enabled: root.filtering
            anchors.left: parent.left
            anchors.right: countText.left
            anchors.leftMargin: Style.space(18)
            anchors.verticalCenter: parent.verticalCenter
            color: root.bar.foreground
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
            text: root.filterText
            onTextChanged: root.filterText = text
          }

          Text {
            id: countText
            textFormat: Text.PlainText
            text: Commons.I18n.formatNumber(root.rows.length)
            color: Qt.darker(root.bar.foreground, 1.6)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
            anchors.right: parent.right
            anchors.rightMargin: Style.space(6)
            anchors.verticalCenter: parent.verticalCenter
          }
        }
      }

      ListView {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.topMargin: Style.space(6)
        anchors.bottom: parent.bottom
        clip: true
        spacing: Style.space(2)
        model: root.rows
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: list.contentHeight > list.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

        delegate: CursorSurface {
          id: productRow
          required property var modelData
          required property int index
          readonly property bool here: root.selectedIndex === index
          readonly property bool detail: root.detailIndex === index
          width: list.width
          hasCursor: here
          foreground: root.bar.foreground
          implicitHeight: productColumn.implicitHeight + Style.spacing.md

          Column {
            id: productColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: Style.space(6)
            anchors.rightMargin: Style.space(6)
            spacing: Style.space(2)

            Item {
              width: parent.width
              implicitHeight: productName.implicitHeight
              Text {
                id: productName
                textFormat: Text.PlainText
                text: productRow.modelData.name
                color: root.bar.foreground
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.body
                font.bold: productRow.here
                elide: Text.ElideRight
                anchors.left: parent.left
                anchors.right: gradeChip.left
                anchors.rightMargin: Style.space(8)
              }
              Text {
                id: gradeChip
                textFormat: Text.PlainText
                text: Model.gradeLabel(productRow.modelData.worst, root.t)
                readonly property string tone: Model.statusTone(productRow.modelData.worst)
                color: tone === "urgent" ? Commons.Color.urgent : (tone === "warn" ? Commons.Color.accent : Qt.darker(root.bar.foreground, 1.6))
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                anchors.right: parent.right
                anchors.verticalCenter: productName.verticalCenter
              }
            }

            Text {
              visible: productRow.here
              textFormat: Text.PlainText
              text: (productRow.modelData.vendor ? productRow.modelData.vendor + " · " : "") + Model.formatsLine(productRow.modelData)
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption
              width: parent.width
              elide: Text.ElideRight
            }

            Text {
              visible: productRow.here
              textFormat: Text.PlainText
              text: Model.findingLine(productRow.modelData, root.t)
              readonly property string tone: Model.statusTone(productRow.modelData.worst)
              color: tone === "urgent" ? Commons.Color.urgent : (tone === "warn" ? Commons.Color.accent : Qt.darker(root.bar.foreground, 1.4))
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption
              width: parent.width
              wrapMode: Text.Wrap
              maximumLineCount: productRow.detail ? 12 : 2
              elide: Text.ElideRight
            }

            // Enter: every format's grade, how far it was checked, and path.
            Repeater {
              model: productRow.detail ? productRow.modelData.formats : []
              Column {
                required property var modelData
                width: productColumn.width
                spacing: 0
                Text {
                  textFormat: Text.PlainText
                  text: String(modelData.format).toUpperCase() + "  " + Model.gradeLabel(modelData.grade, root.t)
                    + "  · " + (modelData.checked === "load" ? root.t("plugins.checkedLoad", "load checked") : root.t("plugins.checkedRun", "run checked"))
                  color: root.bar.foreground
                  font.family: root.bar.fontFamily
                  font.pixelSize: Style.font.caption
                  font.bold: true
                }
                Text {
                  textFormat: Text.PlainText
                  text: modelData.path + ((modelData.reasons || []).length ? "\n" + modelData.reasons.join("\n") : "")
                  color: Qt.darker(root.bar.foreground, 1.4)
                  font.family: root.bar.fontFamily
                  font.pixelSize: Style.font.caption
                  width: parent.width
                  wrapMode: Text.WrapAnywhere
                }
              }
            }
          }

          MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onContainsMouseChanged: if (containsMouse && root.selectedIndex !== productRow.index) { root.selectedIndex = productRow.index; root.detailIndex = -1 }
            onClicked: { root.selectedIndex = productRow.index; root.activate() }
          }
        }
      }
    }
  }
}
