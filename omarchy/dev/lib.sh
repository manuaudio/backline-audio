# Helpers for driving Backline's real panels on the private headless
# compositor (dev/headless) against the bench (dev/bench). Sourced.
# shellcheck shell=bash

DEV_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$DEV_DIR/../.." && pwd)
# Omarchy's shell to stage the panels against: the installed one by default,
# or an Omarchy checkout's (SHELL_ROOT=<checkout>/shell). The panels need the
# quattro shell's plugin API.
SHELL_ROOT=${SHELL_ROOT:-${OMARCHY_PATH:-/usr/share/omarchy}/shell}

now_ms() { date +%s%3N; }

# stage_shell: a disposable Quickshell config holding Omarchy's real Ui and
# Commons, Backline's four real panels (copied, links resolved, as the
# installer copies them), and a strip standing in for the bar, with Tab
# moving between neighbours the way the bar's switchPanelFrom does.
stage_shell() {
  STAGE=$BACKLINE_BENCH/stage
  rm -rf "$STAGE"
  mkdir -p "$STAGE"
  ln -s "$SHELL_ROOT/Ui" "$STAGE/Ui"
  ln -s "$SHELL_ROOT/Commons" "$STAGE/Commons"
  local p
  for p in instruments plugins record transport; do
    cp -rL "$REPO/omarchy/plugins/backline.$p" "$STAGE/$p"
  done
  cat >"$STAGE/shell.qml" <<'QML'
import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.Commons
import qs.Commons as Commons
import "instruments" as Instruments
import "plugins" as Plugins
import "record" as Record
import "transport" as Transport

ShellRoot {
  PanelWindow {
    id: strip
    anchors { top: true; left: true; right: true }
    implicitHeight: 30
    color: Commons.Color.bar.background
    WlrLayershell.namespace: "omarchy-bar"

    Text {
      textFormat: Text.PlainText
      text: "Backline"
      color: Commons.Color.foreground
      font.family: Style.font.family
      font.pixelSize: Style.font.body
      anchors.left: parent.left
      anchors.leftMargin: 14
      anchors.verticalCenter: parent.verticalCenter
    }

    QtObject {
      id: barApi
      property color foreground: Commons.Color.foreground
      property color barForeground: Commons.Color.foreground
      property color background: Commons.Color.background
      property color urgent: Commons.Color.urgent
      property string fontFamily: Style.font.family
      property string position: "top"
      property int barSize: 30
      property bool vertical: false
      property bool foregroundAnimationEnabled: false
      property var activePopout: null
      property var shell: null
      property var order: []
      function requestPopout(owner) { activePopout = owner }
      function releasePopout(owner) { if (activePopout === owner) activePopout = null }
      function registerClickTarget(target) {}
      function unregisterClickTarget(target) {}
      function hideTooltip(target) {}
      function showTooltip(target, text) {}
      function switchPanelFrom(owner, direction) {
        var i = order.indexOf(owner)
        if (i < 0) return false
        var next = order[(i + direction + order.length) % order.length]
        owner.closeForPopoutSwitch()
        next.open()
        return true
      }
    }

    Row {
      anchors.right: parent.right
      anchors.rightMargin: 12
      anchors.verticalCenter: parent.verticalCenter
      spacing: 2
      Transport.Panel { id: transport; bar: barApi }
      Instruments.Panel { id: instruments; bar: barApi }
      Plugins.Panel { id: plugins; bar: barApi }
      Record.Panel { id: record; bar: barApi }
    }
    Component.onCompleted: barApi.order = [transport, instruments, plugins, record]
  }
}
QML
  export BENCH_QS_PATH=$STAGE
}

# Quickshell with the bench's environment; its log is checked for QML errors.
start_shell() {
  stage_shell
  # The headless seat has no keyboard until one is attached, and Qt only binds
  # a wl_keyboard while the seat has one, so keep a virtual keyboard attached.
  wtype -s 100000000 &
  KEY_HOLDER=$!
  quickshell -p "$STAGE" --no-color >"$BACKLINE_BENCH/logs/quickshell.log" 2>&1 &
  QS_PID=$!
  local _
  for _ in {1..100}; do
    ipc backline.instruments state >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "quickshell did not come up; see $BACKLINE_BENCH/logs/quickshell.log" >&2
  return 1
}

stop_shell() {
  [[ -n ${QS_PID:-} ]] && kill "$QS_PID" 2>/dev/null
  [[ -n ${KEY_HOLDER:-} ]] && kill "$KEY_HOLDER" 2>/dev/null
  QS_PID=""
  KEY_HOLDER=""
}

ipc() {
  quickshell ipc -p "$STAGE" call "$@" 2>&1
}

# panel_state TARGET JQ: one field of a panel's state() as jq sees it.
panel_state() {
  ipc "$1" state | jq -r "$2" 2>/dev/null
}

# key K...: real key presses into the focused surface. A chord is one wtype
# argument list, e.g. key "-M shift -k Return -m shift".
key() {
  local k
  for k in "$@"; do
    if [[ $k == -* ]]; then
      # shellcheck disable=SC2086
      wtype $k
    elif ((${#k} == 1)); then
      wtype "$k"
    else
      wtype -k "$k"
    fi
    sleep 0.12
  done
}

# wait_until TIMEOUT_S COMMAND...: poll until COMMAND succeeds; prints ms.
wait_until() {
  local timeout=$1 start
  shift
  start=$(now_ms)
  while ! "$@" >/dev/null 2>&1; do
    if (($(now_ms) - start > timeout * 1000)); then
      echo "timeout"
      return 1
    fi
    sleep 0.02
  done
  echo $(($(now_ms) - start))
}

shot() { # name [geometry]
  sleep 0.5
  grim ${2:+-g "$2"} "$SHOTS/$1.png"
}

qml_errors() {
  grep -E 'ReferenceError|TypeError|Unable to assign|Binding loop|is not a type|Cannot assign|unavailable' "$BACKLINE_BENCH/logs/quickshell.log"
}
