import QtQuick

// Keys, the service lamps and the meter cost, along the bottom. On a narrow
// window the key hints are cut short (…) before they reach the lamps, and the
// meter cost goes before the lamps do.
Rectangle {
  id: statusLine
  height: 26
  color: theme.surface

  Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: theme.line }

  Text {
    id: hints
    anchors { left: parent.left; leftMargin: 12; right: lamps.left; rightMargin: 16; verticalCenter: parent.verticalCenter }
    textFormat: Text.PlainText
    text: services.lastError !== "" ? services.lastError
      : qsTr("h/l strip · j/k fader · a arm · Space record · Shift+M marker · m mute · p play · ? keys")
    color: services.lastError !== "" ? theme.urgent : theme.dim
    font.pixelSize: 11
    elide: Text.ElideRight
  }

  Row {
    id: lamps
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
      // the meter cost is for measuring, not for playing: it goes first
      visible: statusLine.width >= 760
      textFormat: Text.PlainText
      text: qsTr("METERS %1 Hz · %2% UI").arg(meters.rate).arg(meters.busyPercent.toFixed(2))
      color: theme.dim
      font.pixelSize: 10
      font.letterSpacing: 1
    }
  }
}
