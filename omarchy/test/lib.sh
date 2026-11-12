# Minimal helpers in the shape of Omarchy's test/shell.d/base-test.sh:
# pass/fail/skip with TAP lines, first failure ends the file. Sourced.
# shellcheck shell=bash
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

pass() { printf 'ok - %s\n' "$1"; }
skip() { printf 'ok - %s # SKIP\n' "$1"; }
fail() {
  [[ -n ${2:-} ]] && printf '%s\n' "$2" >&2
  printf 'not ok - %s\n' "$1" >&2
  exit 1
}

# run_node_test < script: the script gets `Model` (plugins/shared/Model.js)
# and `assert`.
run_node_test() {
  {
    printf 'const assert = require("assert");\nconst Model = require(%s);\n' "\"$ROOT/plugins/shared/Model.js\""
    cat
  } | node -
}
