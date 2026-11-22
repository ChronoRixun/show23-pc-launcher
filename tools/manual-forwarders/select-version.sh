#!/bin/sh
set -eu
top="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
exec python3 -B - "$top" <<'PY'
import hashlib
import pathlib
import re
import sys

versions = pathlib.Path(sys.argv[1]) / "show23-pc" / "versions"
number = r"(?:0|[1-9][0-9]{0,8})"
folder_pattern = re.compile(
    rf"(?P<core>{number}\.{number}\.{number})(?:-(?P<pre>[a-z0-9-]+(?:\.[a-z0-9-]+)*))?(?:~(?P<copy>[2-9]|[1-9][0-9]+))?"
)


def folder_key(name):
    match = folder_pattern.fullmatch(name)
    if not match or len(name.split("~")[0]) > 48:
        return None
    suffix = (match["pre"] or "").split(".")
    if any(token.isdigit() and token.startswith("0") and len(token) > 1 for token in suffix):
        return None
    copy = match["copy"] or "1"
    if len(copy) > 10 or int(copy) > 4294967295:
        return None
    prerelease = tuple((0, int(token)) if token.isdigit() else (1, token) for token in suffix)
    return (*map(int, match["core"].split(".")), match["pre"] is None, prerelease, int(copy))


def record(path, limit):
    if path.is_symlink() or not path.is_file():
        return []
    try:
        with path.open("rb") as source:
            data = source.read(limit + 1)
        match = re.fullmatch(rb"(.*\n)end ([a-f0-9]{64})\r?\n?", data, re.DOTALL)
        if len(data) <= limit and match and hashlib.sha256(match[1]).hexdigest().encode() == match[2]:
            return match[1].decode("ascii").splitlines()
    except (OSError, ValueError):
        pass
    return []


def complete(name):
    if folder_key(name) is None:
        return False
    slot = versions / name
    manual = slot / "manual"
    if slot.is_symlink() or manual.is_symlink() or not manual.is_dir():
        return False
    inventory = record(slot / "version.txt", 2 * 1024 * 1024)
    base = name.split("~")[0]
    channel = "beta" if "-" in base else "stable"
    return (
        inventory[:4] == ["show23-pc version 1", "version " + base, "channel " + channel, "bootstrap 1"]
        and len(inventory) > 4
        and any(re.fullmatch(r"file [a-f0-9]{64} [0-9]+ show23-pc-launcher\.exe", line)
                for line in inventory[4:])
    )


def select():
    if versions.is_symlink() or versions.parent.is_symlink() or not versions.is_dir():
        return None
    pointer = record(versions / "pointer.txt", 8192)
    if len(pointer) >= 6 and pointer[0] == "show23-pc pointer 1":
        for line, field in ((pointer[1], "current "), (pointer[3], "trial ")):
            if line.startswith(field) and complete(line[len(field):]):
                return line[len(field):]
    return max((item.name for item in versions.iterdir() if complete(item.name)),
               key=folder_key, default=None)


try:
    selected = select()
except OSError:
    selected = None
if selected is None:
    sys.exit("No complete launcher version was found. Extract the entire release ZIP.")
print(selected)
PY
