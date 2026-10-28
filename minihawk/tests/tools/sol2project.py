#!/usr/bin/env python3
# Converts a quickerNES .sol input sequence into a Chimera project (.chimeraProject).
#
# The witness plays a movie, which is what a user plays back, so the gate
# exercises that path and nothing runs per frame in a script. The project IS
# the movie (Chimera's docs/project.md): one JSON file with the core's name,
# the settings, the header metadata and the input log.
#
# The columns of the log are the ones the machine has. Change waterbox.config's
# controls or the port settings and the movies must be regenerated; the
# goldens say so loudly if you forget.
#
# Supported: a joypad (or nothing) in each port. A test that uses the Arkanoid
# paddles or the Four Score is refused here, because the layout of its columns
# has not been checked against the window; the goldens of those tests are
# local-only anyway.
#
# usage: sol2project.py <packageDir> <rom> <test.json> <sol> <out.chimeraProject>
import hashlib
import io
import json
import os
import sys


def inputs_of(log):
    """A movie's text block ("[Input]", "LogKey:...", its entries, "[/Input]")
    as a project file's "inputs": the key's groups of column names, and one
    entry a frame. A group starts where a '#' begins a name; a name ends at
    '|' (as Chimera splits a key)."""
    lines = log.splitlines()
    out = {}
    key = next((l[len("LogKey:"):] for l in lines if l.startswith("LogKey:")), None)
    if key is not None:
        groups, at = [], 0
        while at < len(key):
            assert key[at] == "#", key
            groups.append([])
            at += 1
            while at < len(key) and key[at] != "#":
                bar = key.index("|", at)
                groups[-1].append(key[at:bar])
                at = bar + 1
        out["columns"] = groups
    out["entries"] = [l for l in lines if l.startswith("|")]
    return out

PROJECT_VERSION = "Chimera Project File v1.1"
# the order of a .sol field, and the letters the movie text uses for the buttons
SOL_ORDER = ["Up", "Down", "Left", "Right", "Start", "Select", "B", "A"]
LETTER = {"Up": "U", "Down": "D", "Left": "L", "Right": "R", "Start": "S", "Select": "s", "B": "B", "A": "A"}
# the order of the columns of a player in the log
COLUMNS = ["A", "B", "Select", "Start", "Up", "Down", "Left", "Right"]


def decode_joypad(s):
    pressed = {name for i, name in enumerate(SOL_ORDER[:len(s)]) if s[i] not in ". "}
    return "".join(LETTER[c] if c in pressed else "." for c in COLUMNS)


def main(argv):
    if len(argv) < 6:
        sys.exit("usage: sol2project.py <packageDir> <rom> <test.json> <sol> <out.chimeraProject>")
    pkg, rom_path, test_path, sol_path, out_path = argv[1:6]
    test = json.load(io.open(test_path, encoding="utf-8-sig"))
    c1 = test.get("Controller 1 Type") or "Joypad"
    c2 = test.get("Controller 2 Type") or "None"
    if c1 not in ("Joypad", "None") or c2 not in ("Joypad", "None"):
        sys.exit("controllers %s/%s are not supported by this converter" % (c1, c2))
    port1, port2 = "gamepad", "none"

    cfg = json.load(io.open(os.path.join(pkg, "waterbox.config"), encoding="utf-8-sig"))
    players = (["P1"] if c1 == "Joypad" else []) + (["P2"] if c2 == "Joypad" else [])
    key = "|".join(p + " " + c for p in players for c in COLUMNS) + "|"

    entries = []
    for raw in io.open(sol_path, encoding="utf-8-sig"):
        line = raw.rstrip("\r\n")
        if not line or line[0] != "|":
            continue
        fields = line.split("|")[1:-1]
        # field 0 is the console (reset/power) field, which the native tester
        # parses and then ignores during replay
        fi = 1
        groups = []
        if c1 == "Joypad":
            groups.append(decode_joypad(fields[fi])); fi += 1
        if c2 == "Joypad":
            groups.append(decode_joypad(fields[fi])); fi += 1
        entries.append("||" + "|".join(groups) + "|")

    rom = open(rom_path, "rb").read()
    log = "[Input]\nLogKey:#" + "#" + key + "\n" + "".join(e + "\n" for e in entries) + "[/Input]\n"
    # The core pin carries the NAME the core is known by and nothing else: a
    # witness movie must replay against whatever build is under test, so
    # pinning a version or a package hash would refuse the very thing the gate
    # exists to check.
    project = {
        "title": os.path.splitext(os.path.basename(rom_path))[0],
        "description": "miniHawk witness (converted from %s)" % os.path.basename(sol_path),
        "core": {"name": cfg["coreName"], "version": "", "sha1": ""},
        "rerecords": 0,
        "settings": {"port1": port1, "port2": port2},
        "headers": {
            "MovieVersion": PROJECT_VERSION,
            "Platform": cfg["systemId"],
            "SHA1": hashlib.sha1(rom).hexdigest().upper(),
            "Author": "miniHawk witness",
        },
        "inputs": inputs_of(log),
    }
    with io.open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(json.dumps(project, indent=2, ensure_ascii=False))
    print("%s: %d frames, port1=%s port2=%s" % (os.path.basename(out_path), len(entries), port1, port2))


main(sys.argv)
