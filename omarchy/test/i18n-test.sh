#!/bin/bash

# Every string a person reads is in a catalog, as Omarchy's i18n-test holds
# it: the panels' English catalog is current with the code, every key the
# commands use exists, and every Text in a panel declares its textFormat.

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

"$ROOT/dev/i18n-extract" --check >/dev/null || fail "the panel catalog is current (run omarchy/dev/i18n-extract)"
pass "the panels' English catalog matches the t() calls in the panels and model"

missing=$(grep -ohE 'bl_t [a-zA-Z.]+' "$ROOT"/bin/* | awk '{print $2}' | sort -u | while read -r key; do
  jq -e --arg k "$key" 'has($k)' "$ROOT/i18n/en.json" >/dev/null || echo "$key"
done)
[[ -z $missing ]] || fail "every command string key is in i18n/en.json" "$missing"
unused=$(jq -r 'keys[]' "$ROOT/i18n/en.json" | while read -r key; do
  grep -q "bl_t $key\b" "$ROOT"/bin/* || echo "$key"
done)
[[ -z $unused ]] || fail "every command catalog key is used" "$unused"
pass "command strings: every key used exists, every key exists for a use"

for panel in "$ROOT"/plugins/backline.*/Panel.qml; do
  texts=$(grep -c '^\s*Text {' "$panel")
  formats=$(grep -c 'textFormat:' "$panel")
  ((formats >= texts)) || fail "$(basename "$(dirname "$panel")"): every Text declares textFormat ($formats of $texts)"
done
pass "every Text in the panels declares its textFormat"
