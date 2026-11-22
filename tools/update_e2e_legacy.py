"""Exercise legacy refusal and extraction routes using real portable archives."""

import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import threading
from zipfile import ZipFile

from update_e2e_window import owned_processes


VERSIONED = "0.1.3-beta.1"
LEGACY_FIXTURE = "0.1.2"
LEGACY_UPDATER = "54aa3fc986caeb9f1498e93eb7f003491b8bbf9f"
REPO = "ChronoRixun/show23-pc-launcher"
FEED = f"/repos/{REPO}/releases?per_page=30"
RELEASE = f"/{REPO}/releases/download/v{VERSIONED}/"
CACHE_KEY = f"release-cache-beta-{REPO}"
SETTINGS = "show23-pc/user/properties.json"
LAUNCHER_LOG = "show23-pc/data/logs/show23-pc.log"
PROBE_MARKER = ".m4-runtime-proof"
SENTINELS = {
    "show23-pc/updates/update.log": b"player's previous update log\n",
    "show23-pc/data/logs/m4-player.log": b"player's previous launcher log\n",
    "manual/m4-player.txt": b"player's manual notes\n",
    "LICENSES/m4-player.txt": b"player's license notes\n",
}


def run(ctx):
    ctx.record("E9", "Older Stable opens by extraction and a fresh extraction restores the Beta", lambda: older_stable(ctx))
    ctx.record("E10", "The legacy updater refuses the required minimum and the actual versioned ZIP layout", lambda: refusal(ctx))
    ctx.record("E11", "The new ZIP works side by side and over a legacy portable folder", lambda: migrate(ctx))


def file_hash(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def snapshot(root, excluded=()):
    return {name: file_hash(path) for path in root.rglob("*") if path.is_file()
            and (name := path.relative_to(root).as_posix()) not in excluded}


def same_snapshot(before, after):
    delta = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
    assert not delta, delta


def write(root, relative, data):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def extract(path, top):
    assert not owned_processes(top), "Close all launcher processes before extracting over the portable copy"
    """Map the ZIP's one top folder onto a fresh or existing disposable folder."""
    with ZipFile(path) as archive:
        files = {}
        for entry in archive.infolist():
            member = PurePosixPath(entry.filename)
            assert (not member.is_absolute() and member.parts and member.parts[0] == path.stem
                    and ".." not in member.parts and "\\" not in entry.filename and ":" not in entry.filename
                    and not stat.S_ISLNK(entry.external_attr >> 16)), entry.filename
            if entry.is_dir():
                continue
            relative = member.relative_to(path.stem).as_posix()
            assert relative != "." and relative.casefold() not in files, entry.filename
            files[relative.casefold()] = (relative, entry)
        assert "show23-pc.exe" in files and "show23-pc/portable.marker" in files, path
        for relative, entry in files.values():
            target = top / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, target.open("wb") as destination:
                shutil.copyfileobj(source, destination)
    return set(files)


def start_tree(ctx, name):
    root = ctx.root / name
    root.mkdir()
    install = root / "install"
    write(install, PROBE_MARKER, b"disposable compatibility test\n")
    write(install, "data/profiles/default/user/m4-save.bin", b"disposable save\x00\xff\n")
    return root, install


def seed_portable(top, channel="stable", offline=True):
    for name, data in SENTINELS.items():
        write(top, name, data)
    settings = {
        "launcher-update-channel": channel,
        "launcher-auto-shortcuts": "false",
        "launcher-offline-mode": "true" if offline else "false",
        "m4-custom-setting": "keep this setting",
    }
    write(top, SETTINGS, json.dumps(settings, indent=2).encode())
    return settings


def legacy_version(path):
    prefix, suffix = "show23-pc-", "-win64-portable"
    assert path.stem.startswith(prefix) and path.stem.endswith(suffix), path
    version = path.stem.removeprefix(prefix).removesuffix(suffix)
    assert version in ("0.1.0", "0.1.1", "0.1.2"), version
    return version


def use_install(exe, top, install, version, bundle, runtime):
    """Call the selected tag's production settings and runtime APIs without starting a game."""
    probe = top / "m4-runtime-proof.exe"
    assert not probe.exists(), probe
    shutil.copyfile(exe, probe)
    try:
        result = subprocess.run([probe, "--m4-runtime-proof", install, bundle, version],
                                cwd=top, capture_output=True, text=True, timeout=120)
    finally:
        probe.unlink()
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert result.stdout.startswith("Verified ") and f"Active runtime {runtime}" in result.stdout, result.stdout
    assert Path(json.loads((top / SETTINGS).read_bytes())["show23-install"]) == install
    assert (install / "manifests/runtime.txt").read_text().strip() == runtime
    return result.stdout.strip()


def kept_sentinels(top):
    return {name: (top / name).read_bytes() for name in (*SENTINELS, SETTINGS)}


def pointer_files(top):
    versions = top / "show23-pc/versions"
    pointer = (versions / "pointer.txt").read_bytes()
    assert f"current {VERSIONED}\n".encode() in pointer and b"trial -\nstarts 0\n" in pointer, pointer
    return {name: (versions / name).read_bytes() for name in ("pointer.txt", "pointer.bak")}


def older_stable(ctx):
    root, install = start_tree(ctx, "e9-older-stable")
    beta, stable = root / "portable-beta", root / "portable-stable"
    old = legacy_version(ctx.legacy_zip)
    bundle = f"versions/{VERSIONED}"
    extract(ctx.normal_zip, beta)
    seed_portable(beta, "beta")
    new_exe_hash = file_hash(beta / "show23-pc.exe")
    initial_save = snapshot(install)
    staged = use_install(ctx.normal_runtime_probe, beta, install, VERSIONED, bundle, "0.1.2")
    assert all(file_hash(install / name) == value for name, value in initial_save.items()), initial_save
    install_before = snapshot(install)
    beta_start = ctx.confirm_normal(beta, VERSIONED)
    pointers = pointer_files(beta)
    before_versions = snapshot(beta / "show23-pc/versions" / VERSIONED)
    beta_before = kept_sentinels(beta)

    extract(ctx.legacy_zip, stable)
    seed_portable(stable)
    old_exe_hash = file_hash(stable / "show23-pc.exe")
    side_result = use_install(ctx.legacy_runtime_probe, stable, install, old, ".", "0.1.2")
    legacy_start = ctx.confirm_legacy(stable)
    same_snapshot(install_before, snapshot(install))
    assert pointer_files(beta) == pointers and kept_sentinels(beta) == beta_before

    overlay_members = extract(ctx.legacy_zip, beta)
    assert file_hash(beta / "show23-pc.exe") == old_exe_hash != new_exe_hash
    assert not overlay_members.intersection(name.casefold() for name in beta_before), overlay_members
    assert pointer_files(beta) == pointers and kept_sentinels(beta) == beta_before
    overlay_result = use_install(ctx.legacy_runtime_probe, beta, install, old, ".", "0.1.2")
    overlay_start = ctx.confirm_legacy(beta)
    assert pointer_files(beta) == pointers and kept_sentinels(beta) == beta_before
    same_snapshot(install_before, snapshot(install))

    extract(ctx.normal_zip, beta)
    assert file_hash(beta / "show23-pc.exe") == new_exe_hash
    assert pointer_files(beta) == pointers and kept_sentinels(beta) == beta_before
    restored_start = ctx.confirm_normal(beta, VERSIONED, already_current=True)
    assert pointer_files(beta) == pointers and kept_sentinels(beta) == beta_before
    same_snapshot(before_versions, snapshot(beta / "show23-pc/versions" / VERSIONED))
    same_snapshot(install_before, snapshot(install))
    return {"legacyArchive": ctx.legacy_zip.name, "legacyVersion": old, "runtimeStaged": staged,
            "sideBySideRuntime": side_result, "overlayRuntime": overlay_result,
            "betaStartup": beta_start, "legacyStartup": legacy_start, "legacyOverlayStartup": overlay_start,
            "restoredStartup": restored_start, "legacyExeSha256": old_exe_hash,
            "restoredBootstrapSha256": new_exe_hash, "pointer": pointers["pointer.txt"].decode("ascii"),
            "compatibleRuntimeAndSaveUnchanged": True, "pointerAndBackupUnchanged": True}


def serve_refusal(exe, top, archive, manifest_bytes, sums):
    sizes = {"launcher.json": len(manifest_bytes), "SHA256SUMS.txt": len(sums), archive.name: archive.stat().st_size}
    feed = json.dumps([{"tag_name": "v" + VERSIONED, "draft": False, "prerelease": True,
                        "assets": [{"name": name, "size": size,
                                    "browser_download_url": "https://github.com" + RELEASE + name}
                                   for name, size in sizes.items()]}]).encode()
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            requests.append(self.path)
            if self.path == RELEASE + archive.name:
                self.send_response(200)
                self.send_header("Content-Length", str(sizes[archive.name]))
                self.end_headers()
                with archive.open("rb") as source:
                    shutil.copyfileobj(source, self.wfile, 1024 * 1024)
                return
            bodies = {FEED: feed, RELEASE + "launcher.json": manifest_bytes, RELEASE + "SHA256SUMS.txt": sums}
            body = bodies.get(self.path)
            if body is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as host:
        thread = threading.Thread(target=host.serve_forever, daemon=True)
        thread.start()
        try:
            env = dict(os.environ, SHOW23_FIXTURE_PORT=str(host.server_port))
            proc = subprocess.run([exe, "-legacy-proof"], cwd=top, timeout=75, env=env)
        finally:
            host.shutdown()
            thread.join(timeout=5)
    return proc.returncode, requests


def refusal(ctx):
    source = ctx.legacy_source.read_bytes()
    # Match the Git text blob while also tracking the untouched checkout bytes.
    git_source = source.replace(b"\r\n", b"\n")
    blob = hashlib.sha1(f"blob {len(git_source)}\0".encode() + git_source).hexdigest()
    assert blob == LEGACY_UPDATER, blob
    fixture_hash = file_hash(ctx.legacy_exe)
    asset = ctx.normal_zip
    raw_manifest = (asset.parent / "launcher.json").read_bytes()
    sums = (asset.parent / "SHA256SUMS.txt").read_bytes()
    manifest = json.loads(raw_manifest)
    assert manifest["min_launcher"] == VERSIONED and manifest["version"] == VERSIONED, manifest
    sha256 = file_hash(asset)
    assert manifest["files"] == [{"path": asset.name, "size": asset.stat().st_size, "sha256": sha256}], manifest
    assert f"{sha256}  {asset.name}" in sums.decode().splitlines(), asset
    wrong_floor = json.dumps({**manifest, "min_launcher": "0.1.0"}).encode()
    variants = (
        ("minimum", raw_manifest, f"This release requires launcher {VERSIONED} or newer to install it.", []),
        ("layout", wrong_floor, "The release's zip is not a portable show23-pc launcher.",
         [RELEASE + "SHA256SUMS.txt", RELEASE + asset.name]),
    )
    results = {}
    for name, manifest_bytes, error, expected_asset_requests in variants:
        root, install = start_tree(ctx, "e10-" + name)
        top = root / "portable-legacy"
        extract(ctx.legacy_zip, top)
        app = top / "show23-pc"
        exe = top / "show23-pc.exe"
        original_exe = file_hash(exe)
        shutil.copyfile(ctx.legacy_exe, exe)
        assert file_hash(exe) == fixture_hash
        write(top, "show23-pc/legacy-proof.marker", b"")
        seed_portable(top, "beta", offline=False)
        for path in ("runtime", "manifests"):
            shutil.copytree(app / path, install / path)
        saved = json.loads((top / SETTINGS).read_bytes())
        saved["show23-install"] = str(install)
        write(top, SETTINGS, json.dumps(saved).encode())
        install_before = snapshot(install)
        result_path = "show23-pc/legacy-proof-result.json"
        excluded = {SETTINGS, result_path, LAUNCHER_LOG}
        before = snapshot(top, excluded)

        code, requests = serve_refusal(exe, top, asset, manifest_bytes, sums)
        status = json.loads((top / result_path).read_bytes())
        assert code == 0 and status["state"] == "failed" and status["error"] == error, status
        assert status["current"] == LEGACY_FIXTURE and status["latest"] == VERSIONED and status["canInstall"], status
        assert status["readyVersion"] == "" and not status["fromCache"], status
        assert requests == [FEED, RELEASE + "launcher.json", *expected_asset_requests], requests
        assert status["done"] == (asset.stat().st_size if expected_asset_requests else 0), status
        assert not (app / "updates/ready").exists() and not list((app / "updates").glob(".staging-*"))
        same_snapshot(before, snapshot(top, excluded))
        same_snapshot(install_before, snapshot(install))
        after = json.loads((top / SETTINGS).read_bytes())
        cached = json.loads(after.pop(CACHE_KEY))
        assert cached["tag_name"] == "v" + VERSIONED and cached["prerelease"], cached
        assert {entry["name"] for entry in cached["assets"]} == {"launcher.json", "SHA256SUMS.txt", asset.name}, cached
        assert after == saved, after
        assert (top / LAUNCHER_LOG).read_text().splitlines() == [f"launcher update {VERSIONED}: {error}"]
        results[name] = {"error": error, "requests": requests, "legacyShippedExeSha256": original_exe,
                         "unchangedPortableFiles": len(before), "unchangedInstallFiles": len(install_before),
                         "assetBytesDownloaded": status["done"], "releaseCacheKeyAdded": CACHE_KEY}
    assert file_hash(ctx.legacy_exe) == fixture_hash and ctx.legacy_source.read_bytes() == source
    return {"legacyArchive": ctx.legacy_zip.name, "legacyUpdaterBlob": blob, "legacyFixtureSha256": fixture_hash,
            "candidateZipSha256": sha256, "variants": results}


def legacy_payload(top):
    app = top / "show23-pc"
    return {name: snapshot(app / name, ("logs/show23-pc.log",) if name == "data" else ())
            for name in ("data", "runtime", "manifests", "updates")}


def migrate(ctx):
    root, install = start_tree(ctx, "e11-migrate")
    old_top, side = root / "portable-legacy", root / "portable-side-by-side"
    old = legacy_version(ctx.legacy_zip)
    version_bundle = f"versions/{VERSIONED}"
    extract(ctx.legacy_zip, old_top)
    seed_portable(old_top)
    runtime = (old_top / "show23-pc/manifests/runtime.txt").read_text().strip()
    old_runtime = use_install(ctx.legacy_runtime_probe, old_top, install, old, ".", runtime)
    save_before = snapshot(install)
    old_exe_hash = file_hash(old_top / "show23-pc.exe")
    old_manual_hash = file_hash(old_top / "manual/play.cmd")
    journal = "show23-pc/updates/applying.json"
    write(old_top, journal, b'{"version":"0.1.2","items":["data"]}\n')
    old_before = snapshot(old_top)
    old_payload = legacy_payload(old_top)
    old_settings = kept_sentinels(old_top)

    extract(ctx.normal_zip, side)
    seed_portable(side, "beta")
    side_runtime = use_install(ctx.normal_runtime_probe, side, install, VERSIONED, version_bundle, runtime)
    side_start = ctx.confirm_normal(side, VERSIONED)
    side_pointer = pointer_files(side)
    same_snapshot(old_before, snapshot(old_top))
    same_snapshot(save_before, snapshot(install))

    extract(ctx.normal_zip, old_top)
    assert file_hash(old_top / "show23-pc.exe") == file_hash(side / "show23-pc.exe") != old_exe_hash
    assert file_hash(old_top / "manual/play.cmd") == file_hash(side / "manual/play.cmd") != old_manual_hash
    assert kept_sentinels(old_top) == old_settings and legacy_payload(old_top) == old_payload
    overlay_runtime = use_install(ctx.normal_runtime_probe, old_top, install, VERSIONED, version_bundle, runtime)
    overlay_start = ctx.confirm_normal(old_top, VERSIONED)
    overlay_pointer = pointer_files(old_top)
    assert kept_sentinels(old_top) == old_settings and legacy_payload(old_top) == old_payload
    same_snapshot(save_before, snapshot(install))
    return {"legacyArchive": ctx.legacy_zip.name, "legacyRuntime": old_runtime,
            "sideBySideRuntime": side_runtime, "overlayRuntime": overlay_runtime,
            "sideBySideStartup": side_start, "overlayStartup": overlay_start,
            "sideBySidePointer": side_pointer["pointer.txt"].decode("ascii"),
            "overlayPointer": overlay_pointer["pointer.txt"].decode("ascii"),
            "unchangedLegacyFiles": {name: len(files) for name, files in old_payload.items()},
            "unchangedSettingsRuntimeAndSave": True, "replacedManualForwarder": True}
