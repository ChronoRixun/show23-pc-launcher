"""Build the versioned portable archive from a Release build and the frozen runtime."""
import argparse
import hashlib
import json
import re
import shutil
import stat
import sys
from contextlib import contextmanager
from uuid import uuid4
from pathlib import Path, PurePosixPath
from zipfile import ZIP_DEFLATED, ZipFile

REPO = Path(__file__).resolve().parents[1]
LEGACY = "0.1.2"
FIRST_VERSIONED = (0, 1, 3)
EXE = "show23-pc-launcher.exe"
MARKERS = ("SHOW23_FIXTURE_PORT", "SHOW23_UPDATE_FIXTURE", "show23-pc-update-fixture-",
           "fixture-mode", "fixture-before-image")
PARTS = {"emulator": "emulator", "server": "server", "profile": "tools/profile"}


def version(value):
    match = re.fullmatch(r"(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})(?:-beta\.(0|[1-9]\d{0,8}))?", value, re.ASCII)
    if not match:
        raise ValueError("Expected a versioned stable or numbered Beta tag.")
    core = tuple(int(part) for part in match.groups()[:3])
    if core < FIRST_VERSIONED:
        raise ValueError("A versioned tag starts at 0.1.3.")
    return (*core, match[4] is None, int(match[4] or 0))


def digest(path):
    with path.open("rb") as source:
        result = hashlib.sha256()
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
        return result.hexdigest()


def safe_path(name):
    if not name or len(name) > 240:
        return False
    for item in name.split("/"):
        if not re.fullmatch(r"[a-zA-Z0-9_. -]+", item) or item in {".", ".."} or item[0] == " " or item[-1] in " .":
            return False
        stem = item.split(".", 1)[0].rstrip().lower()
        if re.fullmatch(r"con|prn|aux|nul|com\d|lpt\d", stem):
            return False
    return True


def tree(folder):
    result = {}
    folded = set()
    for path in sorted(folder.rglob("*")):
        attr = getattr(path.lstat(), "st_file_attributes", 0)
        name = path.relative_to(folder).as_posix()
        if path.is_symlink() or attr & 1024 or not safe_path(name) or name.lower() in folded:
            raise ValueError(f"Unsafe or duplicate package path: {name}")
        folded.add(name.lower())
        if path.is_file():
            result[name] = path
    return result


def verify_build(exe, release):
    image = exe.read_bytes()
    if image[:2] != b"MZ":
        raise ValueError(f"Expected a Windows executable: {exe.name}")
    for marker in MARKERS:
        if marker.encode() in image or marker.encode("utf-16-le") in image:
            raise ValueError(f"Update fixture executable cannot be packaged: {exe.name}")
    # Both executables embed the exact tag in VERSIONINFO.
    if (release + "\0").encode("utf-16-le") not in image:
        raise ValueError(f"Rebuild {exe.name} with SHOW23_PACKAGE_VERSION={release}.")


def read_receipt(path, archive_path):
    receipt = json.loads(path.read_bytes())
    files = receipt.get("files", [])
    if (receipt.get("schema"), receipt.get("part"), receipt.get("version"), receipt.get("channel")) != (1, "launcher", LEGACY, "stable") or len(files) != 1:
        raise ValueError("Supply the 0.1.2 launcher release manifest for the frozen runtime ZIP.")
    asset = files[0]
    if (asset.get("path") != f"show23-pc-{LEGACY}-win64-portable.zip" or
            asset.get("size") != archive_path.stat().st_size or digest(archive_path) != asset.get("sha256")):
        raise ValueError("The frozen runtime archive does not match the 0.1.2 release manifest.")


def copy_frozen(archive_path, release, licenses):
    prefix = f"show23-pc-{LEGACY}-win64-portable/"
    manifest_root = f"manifests/{LEGACY}/"
    runtime_root = f"runtime/{LEGACY}/"
    with ZipFile(archive_path) as archive:
        seen = set()
        for item in archive.infolist():
            if item.is_dir():
                continue
            if item.filename.startswith(prefix + "LICENSES/"):
                root, name = licenses, item.filename.removeprefix(prefix + "LICENSES/")
            elif item.filename.startswith(prefix + "show23-pc/"):
                root, name = release, item.filename.removeprefix(prefix + "show23-pc/")
                if not name.startswith((manifest_root, runtime_root)) and name != "manifests/runtime.txt":
                    continue
            else:
                continue
            key = (root, name.lower())
            if not safe_path(name) or key in seen or stat.S_ISLNK(item.external_attr >> 16):
                raise ValueError(f"Unsafe frozen archive entry: {name}")
            seen.add(key)
            target = root / PurePosixPath(name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(item) as source, target.open("xb") as dest:
                shutil.copyfileobj(source, dest)

    if (release / "manifests/runtime.txt").read_text(encoding="ascii").strip() != LEGACY:
        raise ValueError("The frozen runtime marker must name 0.1.2.")
    manifests = tree(release / manifest_root)
    if set(manifests) != {part + ".json" for part in PARTS}:
        raise ValueError("The frozen runtime part manifest set does not match 0.1.2.")
    files = tree(release / runtime_root)
    expected = set()
    for part, folder in PARTS.items():
        manifest = json.loads(manifests[part + ".json"].read_bytes())
        if (manifest.get("schema"), manifest.get("part"), manifest.get("version")) != (1, part, LEGACY) or not manifest.get("files"):
            raise ValueError(f"Invalid frozen {part} manifest.")
        for asset in manifest["files"]:
            relative = asset["path"]
            name = folder + "/" + relative
            if not safe_path(relative) or name in expected or name not in files:
                raise ValueError(f"Missing or duplicate frozen runtime file: {name}")
            file = files[name]
            if asset["size"] != file.stat().st_size or asset["sha256"] != digest(file):
                raise ValueError(f"Frozen runtime differs from its manifest: {name}")
            expected.add(name)
    if expected != files.keys():
        raise ValueError("The frozen runtime contains files missing from its manifests.")
    if not (licenses / "emulator/SOURCE.txt").is_file():
        raise ValueError("The frozen runtime is missing its emulator source notice.")


def inventory(release, tag, channel):
    lines = [f"show23-pc version 1\nversion {tag}\nchannel {channel}\nbootstrap 1\n"]
    files = tree(release)
    if len(files) > 8192 or any(name.lower() in {"version.txt", ".writing"} for name in files):
        raise ValueError("Version inventory exceeds its limits or contains a reserved entry.")
    for name, file in files.items():
        lines.append(f"file {digest(file)} {file.stat().st_size} {name}\n")
    data = "".join(lines).encode("ascii")
    data += f"end {hashlib.sha256(data).hexdigest()}\n".encode()
    if len(data) > 2 * 1024 * 1024:
        raise ValueError("Version inventory exceeds 2 MiB.")
    (release / "version.txt").write_bytes(data)


def part_manifest(tag, channel, floor, file, name):
    return {"schema": 1, "part": "launcher", "version": tag, "channel": channel,
            "min_launcher": floor, "files": [{"path": name, "size": file.stat().st_size, "sha256": digest(file)}],
            "compatibility": {part: "*" for part in PARTS}}


def save_json(path, data):
    path.write_bytes((json.dumps(data, indent=2) + "\n").encode())


def copy_tree(source, target):
    for name, file in tree(source).items():
        dest = target / PurePosixPath(name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(file, dest)


@contextmanager
def staging(out):
    root = out / (".release-" + uuid4().hex)
    # Use the output directory's permissions, including its inherited ACL.
    root.mkdir()
    try:
        yield root
    finally:
        shutil.rmtree(root)


def package(tag, floor, frozen, receipt, out):
    if version(floor) > version(tag):
        raise ValueError("The first versioned tag cannot be newer than this release.")
    channel = "beta" if "-beta." in tag else "stable"
    bin_path = REPO / "build/bin/x64/Release"
    launcher = bin_path / "show23-pc.exe"
    bootstrap = bin_path / "show23-pc-bootstrap.exe"
    for exe in (launcher, bootstrap):
        verify_build(exe, tag)
    read_receipt(receipt, frozen)

    name = f"show23-pc-{tag}-win64-portable"
    out.mkdir(parents=True, exist_ok=True)
    with staging(out) as root:
        payload = root / name
        app = payload / "show23-pc"
        release = app / "versions" / tag
        release.mkdir(parents=True)
        (app / "portable.marker").touch()
        shutil.copyfile(bootstrap, payload / "show23-pc.exe")
        shutil.copyfile(launcher, release / EXE)
        (release / "bootstrap").mkdir()
        shutil.copyfile(bootstrap, release / "bootstrap/show23-pc.exe")
        cef = REPO / "build/runtime/x64/Release/cef"
        ui = REPO / "src/launcher-ui"
        for required in (cef / "libcef.dll", ui / "main.html"):
            if not required.is_file():
                raise ValueError(f"Release build asset missing: {required.name}")
        copy_tree(cef, release / "cef/release")
        copy_tree(ui, release / "ui")
        licenses = payload / "LICENSES"
        copy_frozen(frozen, release, licenses)
        shutil.copyfile(REPO / "LICENSE", licenses / "launcher-GPL-3.0.txt")
        copy_tree(licenses, release / "LICENSES")
        source = (licenses / "emulator/SOURCE.txt").read_text(encoding="utf-8-sig").strip()
        readme = (REPO / "tools/player-readme.txt").read_text(encoding="utf-8")
        player = readme.replace("@VERSION@", tag).replace("@EMULATOR_SOURCE@", source)
        for folder in (payload, release):
            (folder / "README.txt").write_bytes(player.encode())
        copy_tree(REPO / "tools/manual-forwarders", payload / "manual")
        copy_tree(REPO / "tools/manual", release / "manual")
        for folder in (payload, release):
            shutil.copyfile(REPO / "tools/manual/README.txt", folder / "manual/README.txt")
        save_json(release / "manifests/launcher.json", part_manifest(tag, channel, floor, launcher, EXE))
        inventory(release, tag, channel)
        sums = [f"{digest(file)}  {rel}\n" for rel, file in tree(payload).items()]
        (payload / "SHA256SUMS.txt").write_bytes("".join(sums).encode())
        pending = root / (name + ".zip")
        with ZipFile(pending, "w", ZIP_DEFLATED, compresslevel=6) as archive:
            for rel, file in tree(payload).items():
                archive.write(file, name + "/" + rel)
        manifest = part_manifest(tag, channel, floor, pending, pending.name)
        (root / "SHA256SUMS.txt").write_bytes(f"{manifest['files'][0]['sha256']}  {pending.name}\n".encode())
        save_json(root / "launcher.json", manifest)
        zip_path = out / pending.name
        pending.replace(zip_path)
        for leaf in ("SHA256SUMS.txt", "launcher.json"):
            (root / leaf).replace(out / leaf)
    print(f"{zip_path} ({manifest['files'][0]['size']} bytes, SHA-256 {manifest['files'][0]['sha256']})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    parser.add_argument("min_launcher")
    parser.add_argument("frozen_zip", type=Path)
    parser.add_argument("frozen_manifest", type=Path)
    parser.add_argument("out_dir", type=Path)
    args = parser.parse_args()
    try:
        package(args.version, args.min_launcher, args.frozen_zip, args.frozen_manifest, args.out_dir)
    except (OSError, ValueError, KeyError, TypeError) as error:
        sys.exit(str(error))
