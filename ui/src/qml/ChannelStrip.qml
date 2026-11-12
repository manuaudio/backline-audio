import QtQuick
import Backline
import "Strips.js" as Strips

// One channel strip: kind and name, the arm button, a stereo meter with its
// scale, the fader (a node's volume) or, for an instrument, its MIDI input and
// activity lamp, the peak and fader readouts, and mute.
Item {
  id: strip
  width: 96

  property var strip: ({})
  property var instance: null
  property var volume: null
  property bool armed: false
  property bool recording: false
  property bool selected: false
  property int control: 1
  property bool groupStart: false
  property real midiLitAt: 0
  signal picked(int control)

  function resetClip() { meter.resetClip() }

  readonly property bool hasFader: volume !== null && volume !== undefined
  readonly property real faderDb: hasFader ? Number(volume.db) : 0
  readonly property bool muted: hasFader && !!volume.muted
  readonly property int radius: Math.min(3, theme.radius)

  // Ask for the volume once; wpctl answers for nodes that have one.
  Component.onCompleted: if (strip.strip && strip.strip.kind !== "instrument") services.readVolume(strip.strip.id)

  // A group starts with a hairline and its caption, so inputs, instruments
  // and outputs read as blocks.
  Rectangle {
    visible: strip.groupStart
    x: -2
    width: 1
    anchors { top: parent.top; bottom: parent.bottom }
    color: theme.line
  }

  Rectangle {
    id: body
    anchors.fill: parent
    anchors.leftMargin: strip.groupStart ? 4 : 0
    radius: strip.radius
    color: strip.selected ? theme.raised : theme.surface
    border.width: 1
    border.color: strip.selected ? theme.accent : (strip.armed ? theme.alpha(theme.urgent, 0.55) : theme.line)

    MouseArea { anchors.fill: parent; onPressed: strip.picked(-1) }

    // ---- Header ----
    Column {
      id: header
      anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
      spacing: 1
      Text {
        textFormat: Text.PlainText
        text: Strips.kindCaption(strip.strip.kind)
        color: strip.strip.kind === "instrument" ? theme.accent : theme.dim
        font.pixelSize: 9
        font.bold: true
        font.letterSpacing: 1.2
      }
      Text {
        textFormat: Text.PlainText
        text: strip.strip.label || strip.strip.name || ""
        color: theme.foreground
        font.pixelSize: 11
        font.bold: true
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        width: parent.width
        height: 30
      }
    }

    // ---- Arm ----
    Rectangle {
      id: armButton
      anchors { top: header.bottom; topMargin: 4; horizontalCenter: parent.horizontalCenter }
      width: parent.width - 12
      height: 22
      radius: strip.radius
      color: strip.recording ? theme.urgent : (strip.armed ? theme.alpha(theme.urgent, 0.18) : "transparent")
      border.width: strip.selected && strip.control === 0 ? 2 : 1
      border.color: strip.selected && strip.control === 0 ? theme.accent : (strip.armed ? theme.urgent : theme.line)
      Text {
        anchors.centerIn: parent
        textFormat: Text.PlainText
        text: strip.recording ? qsTr("● REC") : (strip.armed ? qsTr("● ARMED") : qsTr("○ ARM"))
        color: strip.recording ? theme.background : (strip.armed ? theme.urgent : theme.dim)
        font.pixelSize: 10
        font.bold: true
        font.letterSpacing: 1
      }
      MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { strip.picked(0); services.toggleArm(strip.strip.name) } }
    }

    // ---- Readouts: peak hold and fader ----
    Row {
      id: readouts
      anchors { top: armButton.bottom; topMargin: 6; left: parent.left; right: parent.right; leftMargin: 6; rightMargin: 6 }
      spacing: 4
      Rectangle {
        width: (parent.width - 4) / 2
        height: 18
        radius: strip.radius
        color: meter.clipped ? theme.urgent : theme.background
        Text {
          anchors.centerIn: parent
          textFormat: Text.PlainText
          text: Strips.dbText(meter.holdDb)
          color: meter.clipped ? theme.background : (meter.live ? theme.foreground : theme.dim)
          font.pixelSize: 10
          font.bold: true
          font.features: { "tnum": 1 }
        }
        MouseArea { anchors.fill: parent; onClicked: meter.resetClip() }
      }
      Rectangle {
        width: (parent.width - 4) / 2
        height: 18
        radius: strip.radius
        color: theme.background
        Text {
          anchors.centerIn: parent
          textFormat: Text.PlainText
          text: strip.hasFader ? Strips.dbText(strip.faderDb) : "—"
          color: strip.selected && strip.control === 1 ? theme.accent : theme.dim
          font.pixelSize: 10
          font.bold: true
          font.features: { "tnum": 1 }
        }
      }
    }

    // ---- Meter, scale, fader ----
    Item {
      id: well
      anchors { top: readouts.bottom; topMargin: 8; bottom: footer.top; bottomMargin: 8; left: parent.left; right: parent.right; leftMargin: 6; rightMargin: 6 }

      // DESIGN SLOT (meter): colours come from theme.ok/warn/urgent, overridable
      // in ~/.config/backline/design.json; zones and ballistics are MeterItem's
      // constants (kWarnDb, kHotDb, kReleaseDbPerS, kHoldS).
      Meter {
        id: meter
        node: strip.strip.name || ""
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 22
        okColor: theme.ok
        warnColor: theme.warn
        hotColor: theme.urgent
        trackColor: theme.background
        holdColor: theme.foreground
      }

      // The scale, from the same law the meter draws with.
      Item {
        id: tickScale
        anchors { left: meter.right; top: parent.top; bottom: parent.bottom }
        width: 22
        Repeater {
          model: Strips.METER_TICKS
          Text {
            required property var modelData
            textFormat: Text.PlainText
            text: String(Math.abs(modelData))
            color: theme.dim
            font.pixelSize: 8
            x: 3
            // Matches MeterItem: the clip lamp and a gap sit above the scale.
            readonly property real lampTop: Math.min(5, tickScale.height * 0.04) + 2
            y: lampTop + (tickScale.height - lampTop) * (1 - Strips.meterPosition(modelData)) - height / 2
          }
        }
      }

      // Fader: the node's volume on the fader law. A node without one (an
      // instrument) shows its MIDI input and lamp instead.
      Item {
        id: fader
        visible: strip.hasFader
        anchors { left: tickScale.right; right: parent.right; top: parent.top; bottom: parent.bottom }
        readonly property real travel: height - cap.height
        Rectangle {
          anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; bottom: parent.bottom; topMargin: cap.height / 2; bottomMargin: cap.height / 2 }
          width: 4
          radius: 2
          color: theme.background
          border.width: 1
          border.color: theme.line
        }
        // 0 dB mark.
        Rectangle {
          anchors.horizontalCenter: parent.horizontalCenter
          width: parent.width
          height: 1
          color: theme.line
          y: cap.height / 2 + fader.travel * (1 - Strips.faderPosition(0))
        }
        // DESIGN SLOT (fader cap): FX Control's fader look, if it has one.
        Rectangle {
          id: cap
          anchors.horizontalCenter: parent.horizontalCenter
          width: Math.min(parent.width, 30)
          height: 16
          radius: strip.radius
          y: fader.travel * (1 - Strips.faderPosition(strip.faderDb))
          color: strip.muted ? theme.line : theme.foreground
          border.width: strip.selected && strip.control === 1 ? 2 : 0
          border.color: theme.accent
          Rectangle { anchors.centerIn: parent; width: parent.width - 8; height: 2; color: theme.background }
        }
        MouseArea {
          anchors.fill: parent
          cursorShape: Qt.SizeVerCursor
          preventStealing: true
          function set(y) { services.setVolumeDb(strip.strip.id, Strips.faderDb(1 - (y - cap.height / 2) / fader.travel)) }
          onPressed: function(mouse) { strip.picked(1); set(mouse.y) }
          onPositionChanged: function(mouse) { if (pressed) set(mouse.y) }
          onDoubleClicked: services.setVolumeDb(strip.strip.id, 0)
          onWheel: function(wheel) { services.setVolumeDb(strip.strip.id, strip.faderDb + (wheel.angleDelta.y > 0 ? 1 : -1)) }
        }
      }

      Column {
        visible: !strip.hasFader
        anchors { left: tickScale.right; right: parent.right; top: parent.top }
        spacing: 6
        // MIDI activity: lit for 150 ms after the instrument's counter moved.
        Rectangle {
          id: midiLamp
          anchors.horizontalCenter: parent.horizontalCenter
          width: 12; height: 12; radius: 6
          property real now: Date.now()
          color: strip.midiLitAt > 0 && now - strip.midiLitAt < 150 ? theme.accent : theme.background
          border.width: 1
          border.color: theme.line
          onVisibleChanged: now = Date.now()
          Connections { target: services; function onMidiActivityChanged() { midiLamp.now = Date.now(); midiFade.restart() } }
          Timer { id: midiFade; interval: 160; onTriggered: midiLamp.now = Date.now() }
        }
        Text {
          anchors.horizontalCenter: parent.horizontalCenter
          textFormat: Text.PlainText
          text: qsTr("MIDI")
          color: theme.dim
          font.pixelSize: 8
          font.bold: true
        }
        Text {
          width: parent.width
          textFormat: Text.PlainText
          text: strip.instance ? Strips.midiText(strip.instance["midi-follow"]) : ""
          color: theme.dim
          font.pixelSize: 8
          wrapMode: Text.Wrap
          maximumLineCount: 3
          elide: Text.ElideRight
          horizontalAlignment: Text.AlignHCenter
        }
        Text {
          width: parent.width
          visible: !!strip.instance && strip.instance.state !== "running"
          textFormat: Text.PlainText
          text: strip.instance ? String(strip.instance.state).toUpperCase() : ""
          color: strip.instance && strip.instance.state === "failed" ? theme.urgent : theme.warn
          font.pixelSize: 8
          font.bold: true
          horizontalAlignment: Text.AlignHCenter
        }
      }
    }

    // ---- Footer: mute ----
    Rectangle {
      id: footer
      anchors { bottom: parent.bottom; bottomMargin: 6; horizontalCenter: parent.horizontalCenter }
      width: parent.width - 12
      height: 22
      radius: strip.radius
      opacity: strip.hasFader ? 1 : 0.35
      color: strip.muted ? theme.warn : "transparent"
      border.width: strip.selected && strip.control === 2 ? 2 : 1
      border.color: strip.selected && strip.control === 2 ? theme.accent : theme.line
      Text {
        anchors.centerIn: parent
        textFormat: Text.PlainText
        text: qsTr("MUTE")
        color: strip.muted ? theme.background : theme.dim
        font.pixelSize: 10
        font.bold: true
        font.letterSpacing: 1
      }
      MouseArea { anchors.fill: parent; enabled: strip.hasFader; cursorShape: Qt.PointingHandCursor; onClicked: { strip.picked(2); services.toggleMute(strip.strip.id) } }
    }
  }
}
