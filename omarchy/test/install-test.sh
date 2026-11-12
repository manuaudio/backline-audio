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

# An install from before 0.4 (four plugins) is replaced, not left beside.
mkdir -p "$HOME/.config/omarchy/plugins/backline.record"
echo '{}' >"$HOME/.config/omarchy/plugins/backline.record/manifest.json"

# Without a yes, setup changes nothing in the user's files.
"$ROOT/bin/omarchy-backline-setup" --no-plugin </dev/null >/dev/null
cmp -s "$HOME/.config/omarchy/extensions/omarchy-menu.jsonc" "$scratch/menu.orig" &&
  cmp -s "$HOME/.config/hypr/bindings.lua" "$scratch/bindings.orig" || fail "setup without a yes leaves the user's files alone"
pass "setup: asks before touching the menu or the bindings"

"$ROOT/install" --yes >/dev/null
"$ROOT/install" --yes >/dev/null

plugin=$HOME/.config/omarchy/plugins/io.github.manuaudio.backline
[[ -f $plugin/manifest.json && -f $plugin/omarchy/plugin/Backline.qml && -f $plugin/omarchy/plugin/Model.js && -f $plugin/omarchy/plugin/i18n/en.json ]] ||
  fail "the plugin is installed with its panels, model and catalog" "$(find "$plugin" | head)"
[[ -z $(find "$plugin" -type l) ]] || fail "the installed plugin has no links"
validator=${OMARCHY_PATH:-/usr/share/omarchy}/bin/omarchy-plugin-validate
if [[ -x $validator ]]; then
  "$validator" "$plugin" >/dev/null || fail "Omarchy's validator accepts the installed plugin"
  "$validator" "$ROOT/.." >/dev/null || fail "Omarchy's validator accepts the repository as a plugin"
fi
[[ ! -e $HOME/.config/omarchy/plugins/backline.record ]] || fail "the old per-panel plugins are taken out"
grep -q "enable io.github.manuaudio.backline" "$scratch/log" || fail "the plugin is enabled through Omarchy" "$(cat "$scratch/log")"
pass "plugin: one bar plugin, valid for Omarchy, enabled; the old four taken out"

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
[[ ! -e $plugin && ! -e $HOME/.local/bin/omarchy-backline-record ]] || fail "uninstall removes the plugin and commands"
pass "uninstall: takes out exactly what install put in"
