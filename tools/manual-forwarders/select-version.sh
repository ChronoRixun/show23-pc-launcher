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


def inventory(name):
    if folder_key(name) is None:
        return []
    slot = versions / name
    manual = slot / "manual"
    if slot.is_symlink() or manual.is_symlink() or not manual.is_dir():
        return []
    lines = record(slot / "version.txt", 2 * 1024 * 1024)
    base = name.split("~")[0]
    channel = "beta" if "-" in base else "stable"
    return lines[4:] if lines[:4] == ["show23-pc version 1", "version " + base, "channel " + channel, "bootstrap 1"] else []


def verified(name):
    slot = versions / name
    expected = {"version.txt"}
    for line in inventory(name):
        match = re.fullmatch(r"file ([a-f0-9]{64}) (0|[1-9][0-9]*) ([a-zA-Z0-9._ /-]+)", line)
        if not match:
            return False
        raw = match[3]
        parts = raw.split("/")
        if (raw in expected or any(p in ("", ".", "..") or p.endswith((" ", ".")) for p in parts)):
            return False
        path = slot
        for part in parts:
            path = path / part
            if path.is_symlink():
                return False
        if not path.is_file() or path.stat().st_size != int(match[2]):
            return False
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != match[1]:
            return False
        expected.add(raw)
    if "show23-pc-launcher.exe" not in expected:
        return False
    actual = set()
    for path in slot.rglob("*"):
        if path.is_symlink():
            return False
        if path.is_file():
            actual.add(path.relative_to(slot).as_posix())
    return actual == expected


def select():
    if versions.is_symlink() or versions.parent.is_symlink() or not versions.is_dir():
        return None
    for record_name in ("pointer.txt", "pointer.bak"):
        pointer = record(versions / record_name, 8192)
        if len(pointer) >= 6 and pointer[0] == "show23-pc pointer 1":
            for line, field in ((pointer[1], "current "), (pointer[3], "trial ")):
                if line.startswith(field) and line[len(field):] != "-":
                    name = line[len(field):]
                    return name if folder_key(name) and verified(name) else None
            break
    newest = max((item.name for item in versions.iterdir() if inventory(item.name)),
                 key=folder_key, default=None)
    return newest if newest and verified(newest) else None


try:
    selected = select()
except OSError:
    selected = None
if selected is None:
    sys.exit("No verified launcher version is ready. Start the launcher or re-extract the complete ZIP.")
print(selected)
PY
