import QtQuick
import "Strips.js" as Strips

// Transport across the top: the record block (state, elapsed, take), the
// timecode with bars|beats, tempo and meter, the sync source and its lock.
Rectangle {
  id: bar
  height: 88
  color: theme.surface

  readonly property var rec: services.recorder
  readonly property string recState: rec.State || "idle"
  readonly property bool recording: recState === "recording"
  readonly property var clock: services.clock
  readonly property var report: services.transport
  readonly property real fps: Number(clock.Fps || 25)
  readonly property real rate: Number(clock.Rate || 48000)
  property real nowMs: Date.now()

  // The timecode moves at 25 Hz between the clock's 4 Hz reports, only while
  // rolling and on screen.
  Timer {
    interval: 40
    repeat: true
    running: !!bar.report.rolling && win.shown
    onTriggered: bar.nowMs = Date.now()
  }
  readonly property real frame: Number(report.frame || 0) + (report.rolling ? Math.max(0, nowMs - Number(report.atMs || nowMs)) / 1000 * rate : 0)

  Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: theme.line }

  // ---- Record block ----
  // DESIGN SLOT (record block): the record control's shape and the take/elapsed
  // layout, if RecSync's own differs.
  Rectangle {
    id: recBlock
    anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 10 }
    width: 300
    radius: Math.min(4, theme.radius)
    color: bar.recording ? theme.alpha(theme.urgent, 0.16) : theme.raised
    border.width: 1
    border.color: bar.recording || bar.recState === "armed" ? theme.urgent : theme.line

    Rectangle {
      id: recLamp
      anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
      width: 26; height: 26; radius: 13
      color: bar.recording ? theme.urgent : "transparent"
      border.width: 2
      border.color: bar.recState === "idle" ? theme.dim : theme.urgent
      MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: services.toggleRecord() }
    }

    Column {
      anchors { left: recLamp.right; leftMargin: 14; verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 10 }
      spacing: 2
      Text {
        textFormat: Text.PlainText
        text: bar.recording ? qsTr("RECORDING") : (bar.recState === "armed" ? qsTr("ARMED · %1 TRACKS · %2 S PRE-ROLL").arg(services.armed.length).arg(services.preRoll) : qsTr("READY"))
        color: bar.recState === "idle" ? theme.dim : theme.urgent
        font.pixelSize: 10
        font.bold: true
        font.letterSpacing: 1.2
      }
      Text {
        textFormat: Text.PlainText
        text: Strips.elapsed(bar.recording && Number(bar.rec.Rate) > 0 ? Number(bar.rec.Frames || 0) / Number(bar.rec.Rate) : 0)
        color: bar.recording ? theme.foreground : theme.dim
        font.pixelSize: 22
        font.bold: true
        font.features: { "tnum": 1 }
      }
      Text {
        textFormat: Text.PlainText
        text: bar.rec.Take || (services.recorderRunning ? qsTr("Space records the armed strips") : qsTr("Recorder starts on first use"))
        color: theme.dim
        font.pixelSize: 10
        elide: Text.ElideRight
        width: parent.width
      }
    }
  }

  // ---- Timecode ----
  Column {
    id: tcBlock
    anchors { left: recBlock.right; leftMargin: 28; verticalCenter: parent.verticalCenter }
    spacing: 0
    Text {
      textFormat: Text.PlainText
      text: services.clockRunning ? Strips.timecode(bar.frame, bar.rate, bar.fps) : "--:--:--:--"
      color: bar.report.rolling ? theme.foreground : theme.mix(theme.dim, theme.foreground, 0.5)
      font.pixelSize: 38
      font.bold: true
      font.features: { "tnum": 1 }
    }
    Row {
      spacing: 18
      Text {
        textFormat: Text.PlainText
        text: services.clockRunning ? String(bar.report.bbt || bar.clock.Bbt || "1|1|0") : qsTr("CLOCK STOPPED · p STARTS IT")
        color: theme.dim
        font.pixelSize: 11
        font.bold: true
        font.letterSpacing: 1
        font.features: { "tnum": 1 }
      }
    }
  }

  // ---- Tempo, meter, sync ----
  Row {
    anchors { left: tcBlock.right; leftMargin: 36; verticalCenter: parent.verticalCenter }
    spacing: 28

    Column {
      spacing: 2
      Text { textFormat: Text.PlainText; text: qsTr("TEMPO"); color: theme.dim; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }
      Text {
        textFormat: Text.PlainText
        text: services.clockRunning ? Number(bar.clock.Tempo || 0).toFixed(1) : "—"
        color: theme.foreground; font.pixelSize: 22; font.bold: true; font.features: { "tnum": 1 }
      }
    }
    Column {
      spacing: 2
      Text { textFormat: Text.PlainText; text: qsTr("METER"); color: theme.dim; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }
      Text {
        textFormat: Text.PlainText
        text: services.clockRunning ? Number(bar.clock.BeatsPerBar || 4) + "/" + Number(bar.clock.BeatType || 4) : "—"
        color: theme.foreground; font.pixelSize: 22; font.bold: true
      }
    }
    Column {
      spacing: 2
      Text { textFormat: Text.PlainText; text: qsTr("SYNC"); color: theme.dim; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }
      Row {
        spacing: 8
        // The lock lamp: ok when locked, warn when chasing without lock.
        Rectangle {
          readonly property bool chasing: bar.clock.LtcMode === "chase" || bar.clock.MtcMode === "chase"
          visible: chasing || !!bar.clock.LinkEnabled
          anchors.verticalCenter: parent.verticalCenter
          width: 10; height: 10; radius: 5
          color: chasing ? (bar.clock.Locked ? theme.ok : theme.warn) : (Number(bar.clock.LinkPeers || 0) > 0 ? theme.ok : theme.line)
        }
        Text {
          textFormat: Text.PlainText
          text: !services.clockRunning ? "—"
            : (bar.clock.Source === "ltc" ? "LTC" : bar.clock.Source === "mtc" ? "MTC" : bar.clock.Source === "link" ? "LINK" : qsTr("INT"))
              + ((bar.clock.LtcMode === "chase" || bar.clock.MtcMode === "chase") ? (bar.clock.Locked ? qsTr(" LOCKED") : qsTr(" NO LOCK")) : "")
          color: theme.foreground; font.pixelSize: 22; font.bold: true
        }
      }
    }
  }

  // ---- The theme's name, so the design-token mapping is visible ----
  // DESIGN SLOT (wordmark): Manu's product wordmark replaces the BACKLINE text
  // here; see ui/DESIGN-LANGUAGE.md, "Where Manu's design slots in".
  Column {
    anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
    spacing: 2
    Text {
      anchors.right: parent.right
      textFormat: Text.PlainText
      text: "BACKLINE"
      color: theme.foreground
      font.pixelSize: 13
      font.bold: true
      font.letterSpacing: 3
    }
    Text {
      anchors.right: parent.right
      textFormat: Text.PlainText
      text: (Number(bar.clock.Rate || bar.rec.Rate || 0) > 0 ? (Number(bar.clock.Rate || bar.rec.Rate) / 1000) + " kHz · " : "") + String(theme.name || "").toUpperCase()
      color: theme.dim
      font.pixelSize: 10
      font.letterSpacing: 1
    }
  }
}
