import QtQuick
import qs.Ui

// Backline's one Omarchy plugin (io.github.manuaudio.backline): a bar widget
// that holds the four Backline panels side by side, each with its own bar
// icon, popup and IPC target (backline.transport, backline.instruments,
// backline.plugins, backline.record), so hotkeys and the commands' refresh
// pushes reach each panel by name.
//
// Tab inside a panel moves to its neighbour here; past the last (or before
// the first) it goes to the bar's next panel, and when the bar has no other
// panel it wraps around these four.
BarWidget {
  id: root

  readonly property var panels: [transport, instruments, plugins, record]
  readonly property bool opened: transport.opened || instruments.opened || plugins.opened || record.opened

  // The shell's summon/hide/toggle for this plugin id, and the bar's own
  // Tab from a neighbouring widget, open the first panel.
  function open() { if (!opened) panels[0].open() }
  function close() {
    for (var i = 0; i < panels.length; i++) if (panels[i].opened) panels[i].close()
  }
  function toggle() { opened ? close() : open() }
  function closeForPopoutSwitch() {
    for (var i = 0; i < panels.length; i++) if (panels[i].opened) panels[i].closeForPopoutSwitch()
  }

  function switchFrom(owner, direction) {
    var i = panels.indexOf(owner)
    if (i < 0) return false
    var step = direction < 0 ? -1 : 1
    var j = i + step
    if (j < 0 || j >= panels.length) {
      if (bar && typeof bar.switchPanelFrom === "function" && bar.switchPanelFrom(root, direction))
        return true
      j = (j + panels.length) % panels.length
    }
    if (panels[j] === owner) return false
    owner.closeForPopoutSwitch()
    panels[j].open()
    return true
  }

  implicitWidth: row.implicitWidth
  implicitHeight: row.implicitHeight

  // Side by side on a horizontal bar, stacked on a vertical one.
  Grid {
    id: row
    anchors.centerIn: parent
    columns: root.vertical ? 1 : 4
    spacing: 2
    Transport { id: transport; bar: root.bar; host: root }
    Instruments { id: instruments; bar: root.bar; host: root }
    Plugins { id: plugins; bar: root.bar; host: root }
    Record { id: record; bar: root.bar; host: root }
  }
}
