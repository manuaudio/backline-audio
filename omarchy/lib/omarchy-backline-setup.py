#!/usr/bin/env python3
"""The file edits behind omarchy-backline-setup, done so they can be undone
byte for byte.

  omarchy-backline-setup.py hotkeys FILE SOURCE [--panels] [--dry-run]
  omarchy-backline-setup.py menu FILE SOURCE [--panels] [--dry-run]
  omarchy-backline-setup.py strip FILE...
  omarchy-backline-setup.py taken KEYS...     (KEYS like "SUPER + CTRL + I")

Backline's lines go between a begin and an end marker. The begin marker says
what else was added with the block (the newline before it, the braces of an
empty menu file, the file itself), so `strip` takes out exactly what was put
in. A file is written only when its content changes, after a timestamped
backup (FILE.bak.backline-YYYYmmdd-HHMMSS), so running setup twice changes
nothing the second time.

Hotkeys are checked against the keys already bound, from the running
Hyprland (hyprctl binds -j) or, without one, from Omarchy's default bindings
and the user's own files; Backline's own block doesn't count. A key that is
taken is not bound, and is reported. Lines in SOURCE tagged `-- panels` need
the bar panels and are kept only with --panels; lines tagged `-- no-panels`
are their stand-ins without them. Menu rows whose action opens a panel
(omarchy-shell -q backline.*) are kept only with --panels.
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

BEGIN = re.compile(r"^[ \t]*(--|//) backline:begin(.*)$")
END = re.compile(r"^[ \t]*(--|//) backline:end[ \t]*$")
TAG = re.compile(r"[ \t]+--[ \t]+(panels|no-panels)[ \t]*$")
NOTE = "added by omarchy-backline-setup"
NL = " (and the newline before it)"
FILE = " (and this file)"
BRACES = " (and the braces)"
MODS = {"SHIFT": 1, "CAPS": 2, "CTRL": 4, "CONTROL": 4, "ALT": 8, "MOD2": 16, "MOD3": 32,
        "SUPER": 64, "WIN": 64, "LOGO": 64, "MOD4": 64, "MOD5": 128}
BIND = re.compile(r'\b(?:o|hl)\.bind\w*\(\s*"([^"]+)"\s*(?:,\s*(?:"((?:[^"\\]|\\.)*)"|nil))?')
UNBIND = re.compile(r'\bhl\.unbind\(\s*"([^"]+)"\s*\)')


def read(path):
    try:
        with open(path, newline="") as f:
            return f.read()
    except FileNotFoundError:
        return None


def strip_blocks(text):
    """The text without any Backline block, and whether the file was ours."""
    lines = text.splitlines(keepends=True)
    out, i, created = [], 0, False
    while i < len(lines):
        m = BEGIN.match(lines[i].rstrip("\r\n"))
        if not m:
            out.append(lines[i])
            i += 1
            continue
        j = i + 1
        while j < len(lines) and not END.match(lines[j].rstrip("\r\n")):
            j += 1
        flags = m.group(2)
        if NL in flags or (flags.strip() == "" and m.group(1) == "--"):
            # the newline setup put before the block; a block from before
            # these markers (a bare "-- backline:begin") always had one
            if out and out[-1].endswith("\n"):
                out[-1] = out[-1][:-1]
        created = created or FILE in flags
        i = j + 1
    return "".join(out), created


def keyspec(spec):
    parts = [p.strip().upper() for p in spec.split("+") if p.strip()]
    if not parts:
        return None
    mask = 0
    for p in parts[:-1]:
        if p not in MODS:
            return None
        mask |= MODS[p]
    return mask, parts[-1]


def pretty(spec):
    return " + ".join(p.strip() for p in spec.split("+"))


def own_binds(text):
    """(mask, KEY, description) of every bind in the file's Backline block."""
    own, inside = [], False
    for line in (text or "").splitlines():
        if BEGIN.match(line):
            inside = True
        elif END.match(line):
            inside = False
        elif inside:
            m = BIND.search(line)
            if m and keyspec(m.group(1)):
                own.append(keyspec(m.group(1)) + (m.group(2) or "",))
    return own


def live_binds():
    """[(mask, KEY, description)] from the running Hyprland, or None."""
    if os.environ.get("BACKLINE_BINDS_JSON"):
        data = json.load(open(os.environ["BACKLINE_BINDS_JSON"]))
    else:
        if not os.environ.get("HYPRLAND_INSTANCE_SIGNATURE") or not shutil.which("hyprctl"):
            return None
        try:
            r = subprocess.run(["hyprctl", "binds", "-j"], capture_output=True, text=True, timeout=5)
            data = json.loads(r.stdout)
        except (OSError, ValueError, subprocess.SubprocessError):
            return None
    return [(b.get("modmask", 0), (b.get("key") or "").upper(), b.get("description") or "")
            for b in data if isinstance(b, dict) and b.get("key") and not b.get("submap")]


def static_binds(bindings_file):
    """The same, read from Omarchy's default bindings and the user's files."""
    root = os.environ.get("OMARCHY_PATH", "/usr/share/omarchy")
    files = sorted(glob.glob(os.path.join(root, "default/hypr/**/*.lua"), recursive=True))
    files += sorted(glob.glob(os.path.join(os.path.dirname(bindings_file), "*.lua")))
    binds = []
    for f in files:
        text = read(f) or ""
        if os.path.abspath(f) == os.path.abspath(bindings_file):
            text = strip_blocks(text)[0]
        for line in text.splitlines():
            code = line.split("--", 1)[0] if not line.lstrip().startswith("--") else ""
            for m in UNBIND.finditer(code):
                k = keyspec(m.group(1))
                binds = [b for b in binds if b[:2] != k]
            for m in BIND.finditer(code):
                k = keyspec(m.group(1))
                if k:
                    binds.append((k[0], k[1], m.group(2) or os.path.basename(f)))
    return binds


def taken_keys(bindings_file):
    """{(mask, KEY): description} of the keys bound by anything but Backline."""
    binds = live_binds()
    if binds is None:
        binds = static_binds(bindings_file)
    else:
        for own in own_binds(read(bindings_file)):  # what Hyprland loaded from our block
            same = [i for i, b in enumerate(binds) if b == own] or [i for i, b in enumerate(binds) if b[:2] == own[:2]]
            if same:
                del binds[same[0]]
    return {b[:2]: b[2] for b in binds if not b[2].startswith("Backline")}


def backup_and_write(path, old, new, created_ok=False):
    if old == new:
        return "unchanged"
    msg = ""
    if old is not None:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        bak, n = f"{path}.bak.backline-{stamp}", 1
        while os.path.exists(bak):
            n += 1
            bak = f"{path}.bak.backline-{stamp}-{n}"
        shutil.copy2(path, bak)
        msg = f"backup: {bak}"
    if new is None:
        os.unlink(path)
        return (msg + "; " if msg else "") + "removed (setup had created it)"
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = f"{path}.backline-tmp"
    with open(tmp, "w", newline="") as f:
        f.write(new)
    if old is not None:
        shutil.copymode(path, tmp)
    os.replace(tmp, path)
    return msg or "created"


def source_lines(source, panels):
    text = read(source) or ""
    lines, inside = [], False
    for line in text.splitlines():
        if BEGIN.match(line):
            inside = True
        elif END.match(line):
            inside = False
        elif inside:
            m = TAG.search(line)
            if m and (m.group(1) == "panels") != panels:
                continue
            lines.append(TAG.sub("", line))
    return lines


def append_block(text, comment, lines, existed):
    """text + a block of lines at its end, recording what else was added."""
    flags = ""
    if not existed:
        flags += FILE
    elif text and not text.endswith("\n"):
        text += "\n"
        flags += NL
    return text + f"{comment} backline:begin: {NOTE}{flags}\n" + "".join(l + "\n" for l in lines) + f"{comment} backline:end\n"


def cmd_hotkeys(path, source, panels, dry):
    old = read(path)
    base, created = strip_blocks(old) if old is not None else ("", False)
    taken = taken_keys(path)
    kept, report = [], []
    for line in source_lines(source, panels):
        m = BIND.search(line)
        k = keyspec(m.group(1)) if m else None
        if k and k in taken:
            report.append(f"hotkey {pretty(m.group(1))}: not bound, it is taken ({taken[k]})")
            continue
        if k:
            report.append(f"hotkey {pretty(m.group(1))}: {m.group(2) or ''}")
        kept.append(line)
    existed = old is not None and not (created and base == "")
    new = append_block(base, "--", kept, existed) if kept else (base if existed else None)
    if new is None and old is None:
        status = "unchanged"
    else:
        status = "unchanged (dry run)" if dry else backup_and_write(path, old, new)
    print("\n".join(report))
    print(f"{path}: {status}")


def cmd_menu(path, source, panels, dry):
    old = read(path)
    base, created = strip_blocks(old) if old is not None else ("", False)
    existed = old is not None and not (created and base == "")
    rows = [l for l in source_lines(source, panels)
            if panels or "omarchy-shell -q backline." not in l]
    end = base.rstrip().rfind("}")
    code = [l for l in base[:end].splitlines() if l.strip() and not l.strip().startswith("//")] if end >= 0 else []
    if end < 0 or "{" not in base:
        # no object to merge into: the block brings its own braces
        new = append_block(base, "//", ["{"] + rows + ["}"], existed)
        new = new.replace(f"// backline:begin: {NOTE}", f"// backline:begin: {NOTE}{BRACES}", 1)
    else:
        head, tail = base[:end], base[end:]
        # The entry before the block may need a comma; it goes inside the
        # block, on its own line, so the user's lines stay as they were.
        if code and not code[-1].rstrip().endswith((",", "{")):
            rows = ["  ,"] + rows
        flags = ""
        if head and not head.endswith("\n"):
            head += "\n"
            flags = NL
        new = head + f"// backline:begin: {NOTE}{flags}\n" + "".join(r + "\n" for r in rows) + "// backline:end\n" + tail
    status = "unchanged (dry run)" if dry else backup_and_write(path, old, new)
    print(f"{path}: {status}")


def cmd_strip(paths):
    for path in paths:
        old = read(path)
        if old is None:
            continue
        new, created = strip_blocks(old)
        if created and new == "":
            new = None
        print(f"{path}: {backup_and_write(path, old, new)}")


def cmd_taken(keys, bindings_file):
    taken = taken_keys(bindings_file)
    bad = 0
    for spec in keys:
        k = keyspec(spec)
        if k is None:
            print(f"{spec}: not a key")
            bad = 1
        elif k in taken:
            print(f"{pretty(spec)}: taken ({taken[k]})")
            bad = 1
        else:
            print(f"{pretty(spec)}: free")
    return bad


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    cmd, args = argv[1], [a for a in argv[2:] if not a.startswith("--")]
    panels, dry = "--panels" in argv, "--dry-run" in argv
    if cmd == "hotkeys" and len(args) == 2:
        cmd_hotkeys(args[0], args[1], panels, dry)
    elif cmd == "menu" and len(args) == 2:
        cmd_menu(args[0], args[1], panels, dry)
    elif cmd == "strip" and args:
        cmd_strip(args)
    elif cmd == "taken" and args:
        bindings = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "hypr/bindings.lua")
        return cmd_taken(args, bindings)
    else:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
