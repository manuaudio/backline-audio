import QtQuick
import QtTest
import Backline
import "../../src/qml"
import "../../src/qml/Strips.js" as Strips

// The strip and its helpers, the way the window uses them: the meter law the
// scale is drawn with must be the C++ one, the fader law must invert, and a
// click on ARM must arm the strip in the set the panels share.
TestCase {
  id: test
  name: "ChannelStrip"
  width: 200
  height: 600
  visible: true
  when: windowShown

  ChannelStrip {
    id: strip
    height: 560
    strip: ({ id: 42, name: "backline.test", label: "Test synth", kind: "instrument" })
  }

  function test_meterLawMatchesCpp() {
    for (var db = -66; db <= 2; db += 0.5)
      fuzzyCompare(Strips.meterPosition(db), meters.position(db), 1e-9, "dB " + db)
  }

  function test_faderLawInverts() {
    for (var db = -60; db <= 6; db += 0.5)
      fuzzyCompare(Strips.faderDb(Strips.faderPosition(db)), db, 1e-9, "dB " + db)
    compare(Strips.faderPosition(0), 0.85)
  }

  function test_readouts() {
    compare(Strips.dbText(-6.04), "−6.0")
    compare(Strips.dbText(0), "0.0")
    compare(Strips.dbText(3), "+3.0")
    compare(Strips.dbText(-80), "−∞")
    compare(Strips.timecode(48000 * 61, 48000, 25), "00:01:01:00")
  }

  function test_midiFollowInWords() {
    compare(Strips.midiText("^(?!Midi-Bridge:Midi Through)"), "ALL MIDI")
    compare(Strips.midiText("^Midi-Bridge:KeyStep 37: KeyStep 37 MIDI 1 \\(capture\\) \\|"), "KeyStep 37")
  }

  function test_groups() {
    compare(Strips.groupStarts([{ kind: "input" }, { kind: "input" }, { kind: "instrument" }, { kind: "output" }]), [0, 2, 3])
  }

  // An instrument strip has no fader (its node has no volume): it shows the
  // MIDI lamp instead, and its mute is disabled.
  function test_instrumentStripHasNoFader() {
    verify(!strip.hasFader)
  }

  function test_clickArmArmsTheSharedSet() {
    var before = services.armed.indexOf("backline.test") >= 0
    mouseClick(strip, strip.width / 2, 62)
    tryCompare(services, "armed", before ? [] : ["backline.test"])
    mouseClick(strip, strip.width / 2, 62)
    tryCompare(services, "armed", before ? ["backline.test"] : [])
  }
}
