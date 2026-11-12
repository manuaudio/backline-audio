#!/bin/bash

# omarchy/install in a fake HOME: what it puts where, that it merges into the
# user's own menu extension and bindings without disturbing them, that a
# second run replaces rather than duplicates, and that --uninstall puts every
# file back exactly as it was.

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
export HOME=$scratch/home
unset XDG_DATA_HOME XDG_CONFIG_HOME
stubs=$scratch/stubs
mkdir -p "$stubs" "$HOME/.config/omarchy/extensions" "$HOME/.config/hypr"
printf '#!/bin/bash\necho "shell $*" >>"%s/log"\n' "$scratch" >"$stubs/omarchy-shell"
printf '#!/bin/bash\necho "enable $*" >>"%s/log"\n' "$scratch" >"$stubs/omarchy-plugin-enable"
printf '#!/bin/bash\necho "disable $*" >>"%s/log"\n' "$scratch" >"$stubs/omarchy-plugin-disable"
chmod +x "$stubs"/*
export PATH="$stubs:$PATH"

# The user's own menu extension, its last entry without a trailing comma.
cat >"$HOME/.config/omarchy/extensions/omarchy-menu.jsonc" <<'JSONC'
// my rows
{
  "trigger.mine": {"label": "Mine", "action": "true"}
}
JSONC
printf -- '-- mine\no.bind("SUPER + H", nil, "voxtype record toggle")\n' >"$HOME/.config/hypr/bindings.lua"
cp "$HOME/.config/omarchy/extensions/omarchy-menu.jsonc" "$scratch/menu.orig"
cp "$HOME/.config/hypr/bindings.lua" "$scratch/bindings.orig"

"$ROOT/install" --yes >/dev/null
"$ROOT/install" --yes >/dev/null

for id in backline.instruments backline.plugins backline.record backline.transport; do
  [[ -f $HOME/.config/omarchy/plugins/$id/manifest.json && -f $HOME/.config/omarchy/plugins/$id/Model.js && ! -L $HOME/.config/omarchy/plugins/$id/Model.js ]] ||
    fail "$id is installed with its model copied in"
  [[ -f $HOME/.config/omarchy/plugins/$id/i18n/en.json ]] || fail "$id carries its catalog"
done
grep -q "enable backline.record" "$scratch/log" || fail "the panels are enabled through Omarchy" "$(cat "$scratch/log")"
pass "panels: installed as self-contained plugins and enabled"

[[ -x $HOME/.local/bin/omarchy-backline-record ]] || fail "the commands are on the PATH"
out=$(HOME=$HOME XDG_CONFIG_HOME=$HOME/.config bash -c 'source "$HOME/.local/share/backline/omarchy/lib/omarchy-backline.sh"; bl_t record.busy')
[[ $out == "Stop the take first" ]] || fail "installed commands find their catalogs" "$out"
pass "commands: linked into ~/.local/bin, with their lib and catalogs"

menu=$HOME/.config/omarchy/extensions/omarchy-menu.jsonc
(($(grep -c 'backline:begin' "$menu") == 1)) || fail "a second run replaces the menu block" "$(cat "$menu")"
# Parsed by Omarchy's own menu parser when its checkout is here, else by the
# same rules (whole-line comments, trailing commas).
model=${OMARCHY_MENU_MODEL:-${OMARCHY_PATH:-/usr/share/omarchy}/shell/plugins/menu/MenuModel.js}
node -e '
const fs = require("fs")
const [file, model] = process.argv.slice(1)
const text = fs.readFileSync(file, "utf8")
let rows
if (fs.existsSync(model)) rows = Object.fromEntries(require(model).parseMenuJsonc(text).map(e => [e.id, e]))
else rows = JSON.parse(text.split("\n").filter(l => !l.trim().startsWith("//")).join("\n").replace(/,(\s*[}\]])/g, "$1"))
if (!rows || !rows["trigger.mine"] || !rows["trigger.backline.record"]) throw new Error("rows missing: " + JSON.stringify(Object.keys(rows || {})))
' "$menu" "$model" || fail "the merged menu parses as the shell parses it, with the user's rows kept" "$(cat "$menu")"
(($(grep -c 'backline:begin' "$HOME/.config/hypr/bindings.lua") == 1)) || fail "the hotkeys are added once"
grep -q 'voxtype' "$HOME/.config/hypr/bindings.lua" || fail "the user's own bindings stay"
pass "menu and hotkeys: merged between markers, once, keeping the user's own"

"$ROOT/install" --uninstall >/dev/null
cmp -s "$menu" "$scratch/menu.orig" || fail "uninstall leaves the menu extension as it was" "$(diff "$scratch/menu.orig" "$menu")"
diff <(sed '/^$/d' "$HOME/.config/hypr/bindings.lua") <(sed '/^$/d' "$scratch/bindings.orig") >/dev/null ||
  fail "uninstall leaves the bindings as they were" "$(cat "$HOME/.config/hypr/bindings.lua")"
[[ ! -e $HOME/.config/omarchy/plugins/backline.record && ! -e $HOME/.local/bin/omarchy-backline-record ]] || fail "uninstall removes the panels and commands"
pass "uninstall: takes out exactly what install put in"
