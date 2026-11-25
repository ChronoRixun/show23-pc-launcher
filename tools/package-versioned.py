"""Build the versioned portable archive from local build, engine and server files."""
import argparse
import csv
import hashlib
import json
import re
import shutil
import sys
from contextlib import contextmanager
from uuid import uuid4
from pathlib import Path, PurePosixPath
from zipfile import ZIP_DEFLATED, ZipFile

REPO = Path(__file__).resolve().parents[1]
FIRST_VERSIONED = (0, 1, 3)
FIRST_NATIVE = "0.2.0"
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
        if not re.fullmatch(r"[a-zA-Z0-9_.+ -]+", item) or item in {".", ".."} or item[0] == " " or item[-1] in " .":
            return False
        stem = item.split(".", 1)[0].rstrip().lower()
        if re.fullmatch(r"con|prn|aux|nul|com\d|lpt\d", stem):
            return False
    return True


def tree(folder):
    attr = getattr(folder.lstat(), "st_file_attributes", 0)
    if not folder.is_dir() or folder.is_symlink() or attr & 1024:
        raise ValueError(f"Expected an ordinary directory: {folder}")
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


ENGINE_REQUIRED = {
    "bb-probe.exe", "bbport.ini", "input.ini", "LICENSE", "prepare.py", "prep/prepare.py",
    "prep/link_modules.py", "prep/link_libc.py", "prep/content_profile.py",
}
SERVER_REQUIRED = {"olserver.py", "roster_decode.py", "card_import.py", "python/python.exe",
                   "tables/ol-messages.csv"}
# Development launch wrappers and build provenance are replaced by the portable entry points and notice.
ENGINE_DEVELOPMENT_FILES = {"play.py", "play.ps1", "SOURCE.txt"}


def copy_engine(package_root, target, notices):
    files = tree(package_root)
    inventory_path = files.pop("MANIFEST.tsv", None)
    if inventory_path is None:
        raise ValueError("The engine package needs MANIFEST.tsv.")
    with inventory_path.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        fields = {"file", "sha256", "msys2_package", "license"}
        if not reader.fieldnames or not fields.issubset(reader.fieldnames) or len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise ValueError("Invalid engine inventory columns.")
        verified = {}
        for row in reader:
            name, checksum = row["file"], row["sha256"]
            if name in verified or name not in files or checksum != digest(files[name]):
                raise ValueError(f"Invalid or damaged engine inventory entry: {name}")
            verified[name] = (checksum, row["msys2_package"], row["license"])
    if verified.keys() != files.keys() or not ENGINE_REQUIRED.issubset(files):
        raise ValueError("The engine inventory must list the entire payload, including the executable, defaults and preparation scripts.")
    output = []
    for name, (checksum, dependency, license_name) in sorted(verified.items()):
        if name in ENGINE_DEVELOPMENT_FILES:
            continue
        installed = target / name
        copy_file(files[name], installed)
        if digest(installed) != checksum:
            raise ValueError(f"The engine file changed while it was being copied: {name}")
        output.append((name, checksum, dependency or "", license_name or ""))
    with (target / "MANIFEST.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(["file", "sha256", "dependency", "license"])
        writer.writerows(output)
    copy_file(target / "MANIFEST.tsv", notices / "DEPENDENCIES.tsv")
    copy_file(files["LICENSE"], notices / "GPL-2.0.txt")
    copy_file(REPO / "tools/engine-source.txt", notices / "SOURCE.txt")
    if any(name.startswith("LICENSES/") for name in files):
        copy_tree(target / "LICENSES", notices / "third-party")


def copy_server(source, target, manual_mode):
    files = tree(source)
    roots = {"python", "vendor", "tables", "replies"}
    for name, path in files.items():
        if name not in SERVER_REQUIRED | {"manual_mode.py", "LICENSE", "LICENSE.txt"} and name.split("/")[0] not in roots:
            raise ValueError(f"Unexpected file in the staged server: {name}")
        if "__pycache__" in name.split("/") or path.suffix.lower() in {".pyc", ".key", ".pem", ".log"}:
            raise ValueError(f"Unexpected generated or private server file: {name}")
        copy_file(path, target / name)
    if not SERVER_REQUIRED.issubset(files) or not any(name.startswith("replies/") for name in files):
        raise ValueError("Supply a staged server with its embedded Python, tables and protocol replies.")
    if manual_mode is not None:
        copy_file(manual_mode, target / "manual_mode.py")
    if not (target / "manual_mode.py").is_file():
        raise ValueError("Supply the current standalone manual script with --manual-mode.")


def copy_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def stage_runtime(release, tag, engine, server, manual_mode=None, server_license=None):
    version(tag)
    channel = "beta" if "-beta." in tag else "stable"
    runtime = release / "runtime" / tag
    manifests = release / "manifests" / tag
    manifests.mkdir(parents=True)
    # The original part key and path are kept for existing manifests and release interfaces.
    copy_engine(engine, runtime / PARTS["emulator"], release / "LICENSES/engine")
    server_target = runtime / PARTS["server"]
    copy_server(server, server_target, manual_mode)
    if server_license is None:
        server_license = next((server_target / name for name in ("LICENSE", "LICENSE.txt")
                               if (server_target / name).is_file()), None)
    if server_license is not None:
        copy_file(server_license, release / "LICENSES/server.txt")
    profile = runtime / PARTS["profile"]
    profile.mkdir(parents=True)
    (profile / "defaults.txt").write_bytes(b"The game creates its profile at first start.\n")
    for part, folder in PARTS.items():
        files = tree(runtime / folder)
        if len(files) > 2048 or any(len(name) > 200 or " " in name for name in files):
            raise ValueError(f"Invalid component inventory: {part}")
        entries = [{"path": name, "size": path.stat().st_size, "sha256": digest(path)} for name, path in files.items()]
        save_json(manifests / (part + ".json"), {
            "schema": 1, "part": part, "version": tag, "channel": channel,
            "min_launcher": tag, "files": entries, "compatibility": {
                "launcher": ">=" + tag, **{other: "=" + tag for other in PARTS if other != part},
            },
        })
        if (manifests / (part + ".json")).stat().st_size > 256 * 1024:
            raise ValueError("Component inventory exceeds 256 KiB.")
    (release / "manifests/runtime.txt").write_bytes((tag + "\n").encode("ascii"))


def launcher_notices(release):
    licenses = {
        "launcher-GPL-3.0.txt": "LICENSE", "browser.txt": "deps/cef/LICENSE.txt",
        "curl.txt": "deps/curl/COPYING", "rapidjson.txt": "deps/rapidjson/license.txt",
        "miniz.txt": "deps/miniz/LICENSE", "libtomcrypt.txt": "deps/libtomcrypt/LICENSE",
        "libtommath.txt": "deps/libtommath/LICENSE",
        "OFL.txt": "src/launcher-ui/assets/fonts/OFL.txt",
    }
    for name, relative in licenses.items():
        copy_file(REPO / relative, release / "LICENSES" / name)


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


SCRIPT_ENDINGS = {".cmd": b"\r\n", ".ps1": b"\r\n", ".sh": b"\n"}


def copy_tree(source, target):
    for name, file in tree(source).items():
        copy_file(file, target / name)


def copy_script(source, target):
    lines = source.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    target.write_bytes(lines.replace(b"\n", SCRIPT_ENDINGS[source.suffix.lower()]))


def check_script_endings(payload):
    for name, file in tree(payload).items():
        eol = SCRIPT_ENDINGS.get(file.suffix.lower())
        if not eol:
            continue
        data = file.read_bytes()
        if eol == b"\r\n":
            data = data.replace(b"\r\n", b"")
            wrong = b"\r" in data or b"\n" in data
        else:
            wrong = b"\r" in data
        if wrong:
            raise ValueError(f"Wrong script line ending in package: {name}")


@contextmanager
def staging(out):
    root = out / (".release-" + uuid4().hex)
    # Use the output directory's permissions, including its inherited ACL.
    root.mkdir()
    try:
        yield root
    finally:
        shutil.rmtree(root)


def package(tag, floor, engine, server, out, manual_mode=None, server_license=None):
    if version(floor) > version(tag):
        raise ValueError("The first versioned tag cannot be newer than this release.")
    channel = "beta" if "-beta." in tag else "stable"
    bin_path = REPO / "build/bin/x64/Release"
    launcher = bin_path / "show23-pc.exe"
    bootstrap = bin_path / "show23-pc-bootstrap.exe"
    for exe in (launcher, bootstrap):
        verify_build(exe, tag)

    name = f"show23-pc-{tag}-win64-native-portable"
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
        stage_runtime(release, tag, engine, server, manual_mode, server_license)
        if not (release / "LICENSES/server.txt").is_file():
            raise ValueError("Supply the staged server licence with --server-license.")
        launcher_notices(release)
        readme = (REPO / "tools/player-readme.txt").read_text(encoding="utf-8")
        player = readme.replace("@VERSION@", tag)
        for folder in (payload, release):
            (folder / "README.txt").write_bytes(player.encode())
        for folder, target in (("manual-forwarders", payload / "manual"), ("manual", release / "manual")):
            target.mkdir()
            for script in (REPO / "tools" / folder).iterdir():
                if script.suffix.lower() in (".cmd", ".ps1"):
                    copy_script(script, target / script.name)
        for folder in (payload, release):
            shutil.copyfile(REPO / "tools/manual/README.txt", folder / "manual/README.txt")
        save_json(release / "manifests/launcher.json", part_manifest(tag, channel, floor, launcher, EXE))
        check_script_endings(payload)
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
    parser.add_argument("--version", default=(REPO / "tools/release-version.txt").read_text(encoding="ascii").strip())
    parser.add_argument("--min-launcher")
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--manual-mode", type=Path)
    parser.add_argument("--server-license", type=Path)
    parser.add_argument("--out", type=Path, default=REPO / "dist")
    parser.add_argument("--stage-only", action="store_true", help="Stage only the runtime into a new output folder")
    args = parser.parse_args()
    try:
        if args.stage_only:
            if args.out.exists():
                raise ValueError("Choose a new output folder for runtime staging.")
            args.out.parent.mkdir(parents=True, exist_ok=True)
            with staging(args.out.parent) as root:
                bundle = root / "bundle"
                stage_runtime(bundle, args.version, args.engine, args.server, args.manual_mode, args.server_license)
                bundle.rename(args.out)
            print(f"Staged runtime {args.version} in {args.out}")
        else:
            package(args.version, args.min_launcher or FIRST_NATIVE, args.engine, args.server, args.out,
                    args.manual_mode, args.server_license)
    except (OSError, ValueError, KeyError, TypeError) as error:
        sys.exit(str(error))
