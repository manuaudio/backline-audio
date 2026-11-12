#!/bin/bash

# omarchy/install in a fake HOME: what it puts where, that it merges into the
# user's own menu extension and bindings without disturbing them, that it
# backs each file up before changing it, that a second run changes nothing,
# that a taken key is never bound, that no row or key points at a panel that
# can't open, and that --uninstall puts every file back byte for byte.

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
export HOME=$scratch/home
unset XDG_DATA_HOME XDG_CONFIG_HOME
# The keys already bound "on this system", as hyprctl binds -j gives them:
# Omarchy 4.0.4's Toggle locking on idle on Super+Ctrl+I, and a user's own
# bind on Super+Ctrl+Alt+G, which Backline must then leave alone.
cat >"$scratch/binds.json" <<'JSON'
[{"modmask": 68, "key": "I", "description": "Toggle locking on idle", "submap": ""},
 {"modmask": 76, "key": "G", "description": "My own thing", "submap": ""},
 {"modmask": 64, "key": "H", "description": "", "submap": ""}]
JSON
export BACKLINE_BINDS_JSON=$scratch/binds.json BACKLINE_PANELS=no
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
cp "$HOME/.config/omarchy/extensions/omarchy-menu.jsonc" "$scratch/menu.once"
cp "$HOME/.config/hypr/bindings.lua" "$scratch/bindings.once"
"$ROOT/install" --yes >/dev/null
cmp -s "$HOME/.config/omarchy/extensions/omarchy-menu.jsonc" "$scratch/menu.once" &&
  cmp -s "$HOME/.config/hypr/bindings.lua" "$scratch/bindings.once" ||
  fail "a second run changes nothing" "$(diff "$scratch/bindings.once" "$HOME/.config/hypr/bindings.lua"; diff "$scratch/menu.once" "$HOME/.config/omarchy/extensions/omarchy-menu.jsonc")"
for f in "$HOME/.config/omarchy/extensions/omarchy-menu.jsonc" "$HOME/.config/hypr/bindings.lua"; do
  backups=("$f".bak.backline-*)
  ((${#backups[@]} == 1)) || fail "one timestamped backup per file, from the first run only" "$(ls "$f".bak.backline-* 2>&1)"
  [[ ${backups[0]} =~ \.bak\.backline-[0-9]{8}-[0-9]{6}$ ]] || fail "the backup is named by date and time" "${backups[0]}"
done
cmp -s "$HOME/.config/omarchy/extensions/omarchy-menu.jsonc".bak.backline-* "$scratch/menu.orig" &&
  cmp -s "$HOME/.config/hypr/bindings.lua".bak.backline-* "$scratch/bindings.orig" || fail "each backup is the file as it was before setup"
pass "setup: backs each file up once, before changing it; a second run changes nothing"

plugin=$HOME/.config/omarchy/plugins/io.github.manuaudio.backline
[[ -f $plugin/manifest.json && -f $plugin/omarchy/plugin/Backline.qml && -f $plugin/omarchy/plugin/Model.js && -f $plugin/omarchy/plugin/i18n/en.json ]] ||
  fail "the plugin is installed with its panels, model and catalog" "$(find "$plugin" | head)"
[[ -z $(find "$plugin" -type l) ]] || fail "the installed plugin has no links"
validator=${OMARCHY_PATH:-/usr/share/omarchy}/bin/omarchy-plugin-validate
if [[ -x $validator ]]; then
  "$validator" "$plugin" >/dev/null || fail "Omarchy's validator accepts the installed plugin"
  # what `omarchy plugin add` clones: the tracked files (a local build/ isn't)
  export_dir=$scratch/export
  mkdir -p "$export_dir"
  git -C "$ROOT/.." archive HEAD | tar -x -C "$export_dir"
  "$validator" "$export_dir" >/dev/null || fail "Omarchy's validator accepts the repository as a plugin"
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

bindings=$HOME/.config/hypr/bindings.lua
! grep -q '"SUPER + CTRL + I"' "$bindings" || fail "Super+Ctrl+I (Omarchy's Toggle locking on idle) is never bound" "$(cat "$bindings")"
! grep -q '"SUPER + CTRL + ALT + G"' "$bindings" || fail "a key the user has bound is not bound again" "$(cat "$bindings")"
grep -q '"SUPER + CTRL + ALT + M"' "$bindings" || fail "the free keys are bound" "$(cat "$bindings")"
out=$("$ROOT/bin/omarchy-backline-setup" --yes --no-menu --no-plugin)
grep -q 'SUPER + CTRL + ALT + G: not bound, it is taken (My own thing)' <<<"$out" || fail "setup says which key it left out, and why" "$out"
pass "hotkeys: checked against the keys already bound; a taken key is refused and reported"

! grep -q 'omarchy-shell -q backline\.' "$bindings" "$menu" || fail "without the panels, no row or key opens one" "$(grep 'omarchy-shell' "$bindings" "$menu")"
grep -q '"SUPER + CTRL + M", "Backline Mixer", "omarchy-backline-mixer"' "$bindings" || fail "the mixer stands in for the panels" "$(cat "$bindings")"
grep -q 'trigger.backline.mixer' "$menu" && grep -q 'trigger.backline.record"' "$menu" || fail "the menu keeps the rows that run commands"
BACKLINE_PANELS=yes "$ROOT/bin/omarchy-backline-setup" --yes --no-plugin >/dev/null
grep -q '"SUPER + CTRL + U", "Backline instruments", "omarchy-shell -q backline.instruments toggle"' "$bindings" &&
  grep -q 'trigger.backline.instruments' "$menu" || fail "with the panels, their keys and rows are added" "$(cat "$bindings")"
! grep -q 'Backline Mixer", "omarchy-backline-mixer"' "$bindings" || fail "with the panels, Super+Ctrl+M is the Record panel"
! grep -q -- '-- panels$' "$bindings" || fail "the tags are not copied"
pass "panels: rows and keys that open a panel only where the panels can load; the mixer and commands otherwise"

"$ROOT/install" --uninstall >/dev/null
cmp -s "$menu" "$scratch/menu.orig" || fail "uninstall leaves the menu extension as it was" "$(diff "$scratch/menu.orig" "$menu")"
cmp -s "$HOME/.config/hypr/bindings.lua" "$scratch/bindings.orig" ||
  fail "uninstall leaves the bindings as they were, byte for byte" "$(diff <(od -c "$scratch/bindings.orig") <(od -c "$HOME/.config/hypr/bindings.lua"))"
[[ ! -e $plugin && ! -e $HOME/.local/bin/omarchy-backline-record ]] || fail "uninstall removes the plugin and commands"
pass "uninstall: takes out exactly what install put in"

# Byte for byte in the awkward cases too: no newline at the end, no file at
# all, and a block written by an older setup (which put a newline before it).
setup=$ROOT/bin/omarchy-backline-setup
printf -- '-- no newline at the end\no.bind("SUPER + H", nil, "x")' >"$bindings"
cp "$bindings" "$scratch/nonl"
rm -f "$menu"
"$setup" --yes --no-plugin >/dev/null
[[ -f $menu ]] || fail "setup creates the menu extension when there is none"
"$setup" --uninstall >/dev/null
cmp -s "$bindings" "$scratch/nonl" || fail "a file without a final newline comes back as it was" "$(od -c "$bindings" | tail -3)"
[[ ! -e $menu ]] || fail "a menu file setup created is removed again" "$(cat "$menu")"
cp "$scratch/bindings.orig" "$bindings"
{ echo; printf -- '-- backline:begin\no.bind("SUPER + CTRL + I", "Instruments", "x")\n-- backline:end\n'; } >>"$bindings"
"$setup" --uninstall >/dev/null
cmp -s "$bindings" "$scratch/bindings.orig" || fail "an older setup's block comes out with the blank line it added" "$(od -c "$bindings" | tail -4)"
pass "uninstall: byte for byte without a final newline, for a file setup created, and for an older setup's block"
