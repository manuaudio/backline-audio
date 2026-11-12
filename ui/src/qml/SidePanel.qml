import QtQuick
import "Strips.js" as Strips

// The right column: the takes (newest first, with markers) and, while a take
// is loaded for soundcheck, each of its inputs live or on playback.
Rectangle {
  id: side
  color: theme.surface

  readonly property var rec: services.recorder
  readonly property string soundcheckTake: rec.Soundcheck || ""
  readonly property var inputs: {
    var list = []
    var map = rec.SoundcheckInputs || {}
    for (var k in map) list.push({ node: k, mode: map[k] })
    return list
  }

  Rectangle { anchors { left: parent.left; top: parent.top; bottom: parent.bottom } width: 1; color: theme.line }

  Column {
    anchors { fill: parent; margins: 14 }
    spacing: 8

    Text {
      textFormat: Text.PlainText
      text: qsTr("TAKES")
      color: theme.dim
      font.pixelSize: 10
      font.bold: true
      font.letterSpacing: 1.4
    }

    Repeater {
      model: services.takes.slice(0, 8)
      Rectangle {
        required property var modelData
        readonly property bool loaded: side.soundcheckTake === modelData.take
        width: parent.width
        height: 44
        radius: Math.min(3, theme.radius)
        color: loaded ? theme.alpha(theme.accent, 0.14) : theme.raised
        border.width: 1
        border.color: loaded ? theme.accent : theme.line
        Column {
          anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 8 }
          spacing: 2
          Text {
            textFormat: Text.PlainText
            text: modelData.take
            color: theme.foreground
            font.pixelSize: 11
            font.bold: true
            elide: Text.ElideRight
            width: parent.width
          }
          Text {
            textFormat: Text.PlainText
            text: Strips.shortLength(modelData.seconds) + "  ·  " + qsTr("%1 tracks").arg((modelData.tracks || []).length)
              + (modelData.markers > 0 ? "  ·  " + qsTr("%1 markers").arg(modelData.markers) : "")
              + "  ·  " + (Number(modelData.rate) / 1000) + " kHz"
            color: theme.dim
            font.pixelSize: 10
            font.features: { "tnum": 1 }
          }
        }
        MouseArea {
          anchors.fill: parent
          cursorShape: Qt.PointingHandCursor
          onClicked: loaded ? services.stopSoundcheck() : services.soundcheck(modelData.take)
        }
      }
    }

    Text {
      visible: services.takes.length === 0
      textFormat: Text.PlainText
      text: services.recorderRunning ? qsTr("No takes yet") : qsTr("The recorder starts when a strip is armed")
      color: theme.dim
      font.pixelSize: 11
      width: parent.width
      wrapMode: Text.Wrap
    }

    Item { width: 1; height: 8 }

    Text {
      visible: side.soundcheckTake !== ""
      textFormat: Text.PlainText
      text: qsTr("SOUNDCHECK")
      color: theme.dim
      font.pixelSize: 10
      font.bold: true
      font.letterSpacing: 1.4
    }

    Repeater {
      model: side.inputs
      Item {
        required property var modelData
        width: parent.width
        height: 26
        Text {
          anchors { left: parent.left; verticalCenter: parent.verticalCenter; right: pills.left; rightMargin: 6 }
          textFormat: Text.PlainText
          text: modelData.node
          color: theme.foreground
          font.pixelSize: 11
          elide: Text.ElideRight
        }
        Row {
          id: pills
          anchors { right: parent.right; verticalCenter: parent.verticalCenter }
          spacing: 3
          Repeater {
            model: ["live", "playback"]
            Rectangle {
              required property var modelData
              readonly property bool on: parent.parent.modelData.mode === modelData
              width: 62; height: 20
              radius: Math.min(3, theme.radius)
              color: on ? (modelData === "playback" ? theme.accent : theme.raised) : "transparent"
              border.width: 1
              border.color: on ? theme.accent : theme.line
              Text {
                anchors.centerIn: parent
                textFormat: Text.PlainText
                text: modelData === "live" ? qsTr("LIVE") : qsTr("PLAYBACK")
                color: parent.on && modelData === "playback" ? theme.accentForeground : (parent.on ? theme.foreground : theme.dim)
                font.pixelSize: 9
                font.bold: true
              }
              MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: services.setInput(parent.parent.parent.modelData.node, modelData) }
            }
          }
        }
      }
    }
  }
}
