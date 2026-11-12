import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Backline
import "Strips.js" as Strips

// The mixer: transport across the top, every channel strip in the middle,
// takes and soundcheck on the right, keys along the bottom. One cursor, a
// strip and a control on it, shared by keyboard and mouse.
ApplicationWindow {
  id: win
  width: 1680
  height: 900
  minimumWidth: 900
  minimumHeight: 560
  visible: true
  title: "Backline"
  color: theme.background
  font.family: theme.fontFamily

  // Meters and the MIDI poll run only while the window can be seen.
  readonly property bool shown: visible && visibility !== Window.Minimized
  Binding { target: meters; property: "active"; value: win.shown }
  Binding { target: services; property: "active"; value: win.shown }

  readonly property var strips: graph.nodes
  property int stripIndex: 0
  // 0 arm, 1 fader, 2 mute
  property int control: 1
  property bool helpVisible: false
  readonly property var currentStrip: strips.length > 0 ? strips[Math.min(stripIndex, strips.length - 1)] : null

  readonly property int unit: Math.max(4, Math.round(theme.fontFamily ? 4 : 4))

  function instanceFor(nodeName) {
    var list = services.instances
    for (var i = 0; i < list.length; i++) if (list[i].node === nodeName) return list[i]
    return null
  }

  function moveStrip(d) { stripIndex = Math.max(0, Math.min(strips.length - 1, stripIndex + d)); stripsView.positionViewAtIndex(stripIndex, ListView.Contain) }
  function moveControl(d) { control = Math.max(0, Math.min(2, control + d)) }
  function armCurrent() { if (currentStrip) services.toggleArm(currentStrip.name) }
  function muteCurrent() { if (currentStrip && services.volumes[String(currentStrip.id)]) services.toggleMute(currentStrip.id) }
  function nudgeFader(db) {
    if (!currentStrip) return
    var v = services.volumes[String(currentStrip.id)]
    if (v) services.setVolumeDb(currentStrip.id, v.db + db)
  }
  function activate() {
    if (control === 0) armCurrent()
    else if (control === 2) muteCurrent()
  }

  Shortcut { sequences: ["h", "Left"]; onActivated: win.moveStrip(-1) }
  Shortcut { sequences: ["l", "Right"]; onActivated: win.moveStrip(1) }
  Shortcut { sequences: ["k", "Up"]; onActivated: win.control === 1 ? win.nudgeFader(1) : win.moveControl(-1) }
  Shortcut { sequences: ["j", "Down"]; onActivated: win.control === 1 ? win.nudgeFader(-1) : win.moveControl(1) }
  Shortcut { sequences: ["Tab"]; onActivated: win.moveControl(win.control === 2 ? -2 : 1) }
  Shortcut { sequences: ["Return", "Enter"]; onActivated: win.activate() }
  Shortcut { sequence: "Space"; onActivated: services.toggleRecord() }
  Shortcut { sequence: "a"; onActivated: win.armCurrent() }
  Shortcut { sequence: "m"; onActivated: win.muteCurrent() }
  Shortcut { sequence: "Shift+M"; onActivated: services.addMarker() }
  Shortcut { sequence: "["; onActivated: win.nudgeFader(-1) }
  Shortcut { sequence: "]"; onActivated: win.nudgeFader(1) }
  Shortcut { sequence: "0"; onActivated: if (win.currentStrip) services.setVolumeDb(win.currentStrip.id, 0) }
  Shortcut { sequence: "p"; onActivated: services.togglePlay() }
  Shortcut { sequence: "Home"; onActivated: services.locate("0") }
  Shortcut { sequence: "c"; onActivated: stripsView.resetClips() }
  Shortcut { sequence: "?"; onActivated: win.helpVisible = !win.helpVisible }
  Shortcut { sequence: "Esc"; onActivated: win.helpVisible = false }
  Shortcut { sequence: "q"; onActivated: Qt.quit() }

  Item {
    anchors.fill: parent
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    TopBar {
      id: topBar
      anchors { left: parent.left; right: parent.right; top: parent.top }
    }

    Rectangle {
      id: stripsArea
      anchors { left: parent.left; right: side.left; top: topBar.bottom; bottom: statusLine.top }
      color: theme.background

      ListView {
        id: stripsView
        anchors.fill: parent
        anchors.margins: 10
        orientation: ListView.Horizontal
        spacing: 2
        clip: true
        model: win.strips
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: ScrollBar { policy: stripsView.contentWidth > stripsView.width ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

        function resetClips() {
          for (var i = 0; i < count; i++) {
            var item = itemAtIndex(i)
            if (item) item.resetClip()
          }
        }

        delegate: ChannelStrip {
          required property var modelData
          required property int index
          height: stripsView.height
          strip: modelData
          instance: win.instanceFor(modelData.name)
          volume: services.volumes[String(modelData.id)] || null
          armed: services.armed.indexOf(modelData.name) >= 0
          recording: services.recorder.State === "recording" && armed
          selected: index === win.stripIndex
          control: win.control
          groupStart: index === 0 || win.strips[index - 1].kind !== modelData.kind
          midiLitAt: services.midiActivity[modelData.name] || 0
          onPicked: function(c) { win.stripIndex = index; if (c >= 0) win.control = c }
        }
      }

      Text {
        anchors.centerIn: parent
        visible: win.strips.length === 0
        textFormat: Text.PlainText
        text: graph.connected ? qsTr("No audio devices or instruments") : qsTr("PipeWire is not running")
        color: theme.dim
        font.pixelSize: 14
      }
    }

    SidePanel {
      id: side
      anchors { right: parent.right; top: topBar.bottom; bottom: statusLine.top }
      width: 330
    }

    // Keys, the service lamps and the meter cost, along the bottom.
    Rectangle {
      id: statusLine
      anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
      height: 26
      color: theme.surface

      Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: theme.line }

      Text {
        anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
        textFormat: Text.PlainText
        text: services.lastError !== "" ? services.lastError
          : qsTr("h/l strip · j/k fader · a arm · Space record · Shift+M marker · m mute · p play · ? keys")
        color: services.lastError !== "" ? theme.urgent : theme.dim
        font.pixelSize: 11
      }

      Row {
        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
        spacing: 14
        Repeater {
          model: [
            { label: qsTr("HOST"), on: services.hostRunning },
            { label: qsTr("REC"), on: services.recorderRunning },
            { label: qsTr("CLOCK"), on: services.clockRunning },
            { label: qsTr("PW"), on: graph.connected }
          ]
          Row {
            required property var modelData
            spacing: 5
            Rectangle {
              width: 6; height: 6; radius: 3
              anchors.verticalCenter: parent.verticalCenter
              color: modelData.on ? theme.ok : theme.line
            }
            Text {
              textFormat: Text.PlainText
              text: modelData.label
              color: modelData.on ? theme.foreground : theme.dim
              font.pixelSize: 10
              font.bold: true
              font.letterSpacing: 1
            }
          }
        }
        Text {
          textFormat: Text.PlainText
          text: qsTr("METERS %1 Hz · %2% UI").arg(meters.rate).arg(meters.busyPercent.toFixed(2))
          color: theme.dim
          font.pixelSize: 10
          font.letterSpacing: 1
        }
      }
    }

    HelpOverlay {
      anchors.fill: parent
      visible: win.helpVisible
    }
  }
}
