import QtQuick

// `?`: every key, as Omarchy's apps list theirs.
Rectangle {
  color: theme.alpha(theme.background, 0.92)
  MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

  Column {
    anchors.centerIn: parent
    spacing: 6
    Text { textFormat: Text.PlainText; text: qsTr("KEYS"); color: theme.dim; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2 }
    Repeater {
      model: [
        ["h / l", qsTr("previous / next strip")],
        ["j / k", qsTr("fader down / up 1 dB (or move between arm, fader, mute)")],
        ["Tab", qsTr("next control on the strip")],
        ["Enter", qsTr("press the control under the cursor")],
        ["a", qsTr("arm or disarm the strip")],
        ["Space", qsTr("record the armed strips, or stop")],
        ["Shift+M", qsTr("marker in the take")],
        ["m", qsTr("mute the strip")],
        ["[ / ]", qsTr("fader down / up 1 dB")],
        ["0", qsTr("fader to 0 dB")],
        ["c", qsTr("clear clip lamps")],
        ["p", qsTr("play or stop the transport")],
        ["Home", qsTr("transport to the start")],
        ["?", qsTr("this list")],
        ["q", qsTr("quit")]
      ]
      Row {
        required property var modelData
        spacing: 16
        Text { textFormat: Text.PlainText; text: modelData[0]; color: theme.accent; font.pixelSize: 13; font.bold: true; width: 90; horizontalAlignment: Text.AlignRight }
        Text { textFormat: Text.PlainText; text: modelData[1]; color: theme.foreground; font.pixelSize: 13 }
      }
    }
  }
}
