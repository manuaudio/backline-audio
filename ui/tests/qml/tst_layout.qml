import QtQuick
import QtTest
import "../../src/qml"

// The transport bar and the status line at the widths a tiled window gets
// (a third, a half, the default), and narrower: every visible text stays
// inside the bar, and no two overlap (the timecode ran into the wordmark,
// and the key hints into the service lamps, at narrow tiled widths).
TestCase {
  id: test
  name: "Layout"
  width: 1680
  height: 200
  visible: true
  when: windowShown

  TopBar { id: top; width: 1680 }
  StatusLine { id: status; y: 120; width: 1680 }

  readonly property var widths: [420, 480, 560, 640, 720, 840, 900, 1000, 1261, 1680]

  function texts(item, out) {
    for (var i = 0; i < item.children.length; i++) {
      var c = item.children[i]
      if (!c.visible || c.opacity === 0) continue
      if (c.text !== undefined && c.paintedWidth !== undefined && c.text !== "") out.push(c)
      texts(c, out)
    }
    return out
  }

  // The rectangle the text paints, in the bar's coordinates.
  function painted(t, root) {
    var p = t.mapToItem(root, 0, 0)
    var w = Math.min(t.paintedWidth, t.width > 0 ? t.width : t.paintedWidth)
    var x = p.x
    if (t.horizontalAlignment === Text.AlignRight) x = p.x + t.width - w
    else if (t.horizontalAlignment === Text.AlignHCenter) x = p.x + (t.width - w) / 2
    var h = Math.min(t.paintedHeight, t.height > 0 ? t.height : t.paintedHeight)
    return { x: x, y: p.y, w: w, h: h, text: t.text }
  }

  function check(root, label) {
    var rects = texts(root, []).map(function(t) { return painted(t, root) })
    verify(rects.length > 0, label + ": texts found")
    for (var i = 0; i < rects.length; i++) {
      var a = rects[i]
      verify(a.x >= -0.5 && a.x + a.w <= root.width + 0.5,
             label + ": \"" + a.text + "\" stays inside (x " + a.x.toFixed(1) + "..." + (a.x + a.w).toFixed(1) + " of " + root.width + ")")
      for (var j = i + 1; j < rects.length; j++) {
        var b = rects[j]
        var overlap = a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 && b.y < a.y + a.h - 0.5
        verify(!overlap, label + ": \"" + a.text + "\" and \"" + b.text + "\" overlap")
      }
    }
  }

  // Columns and Rows lay out on the next polish: let a resize settle.
  function settle(item) {
    wait(60); wait(60)
  }

  function shot(item, name) {
    if (!shotsDir) return
    var done = false
    item.grabToImage(function(r) { r.saveToFile(shotsDir + "/" + name + ".png"); done = true })
    tryVerify(function() { return done }, 2000)
  }

  function test_topBarAtEveryWidth() {
    for (var i = 0; i < widths.length; i++) {
      top.width = widths[i]
      settle(top)
      check(top, "transport bar at " + widths[i] + " px")
      shot(top, "topbar-" + widths[i])
    }
  }

  function test_statusLineAtEveryWidth() {
    for (var i = 0; i < widths.length; i++) {
      status.width = widths[i]
      settle(status)
      check(status, "status line at " + widths[i] + " px")
      shot(status, "status-" + widths[i])
    }
  }

  // Wide, everything is there; narrow, the lower-priority parts give way.
  function test_whatGivesWay() {
    top.width = 1680; settle(top)
    verify(top.showInfo && top.showWordmark, "1680 px: tempo, meter, sync and the wordmark")
    top.width = 480; settle(top)
    verify(!top.showWordmark, "480 px: no wordmark")
  }
}
