"""Run one disposable Windows updater trial against a local releases feed."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
from zipfile import ZIP_DEFLATED, ZipFile

OLD = "0.1.3-beta.1"
NEW = "0.1.3-beta.2"
REPO = "ChronoRixun/show23-pc-launcher"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def release_files(version, exe):
    if b"fixture-mode" not in exe or b"SHOW23_FIXTURE_PORT" not in exe:
        raise ValueError("Both launchers must be isolated update-fixture builds.")
    files = {
        "show23-pc-launcher.exe": exe,
        "ui/main.html": b"Local update fixture\n",
        "manual/play.cmd": f"@echo {version}\r\n".encode(),
        "runtime/0.1.2/server/sentinel.txt": b"frozen runtime\n",
    }
    header = f"show23-pc version 1\nversion {version}\nchannel beta\nbootstrap 1\n"
    body = header + "".join(f"file {digest(data)} {len(data)} {name}\n" for name, data in sorted(files.items()))
    files["version.txt"] = (body + f"end {digest(body.encode())}\n").encode()
    return files


def write_files(root, files):
    for name, data in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def fixture(opts):
    opts.work.mkdir()
    old_files = release_files(OLD, opts.old.read_bytes())
    new_files = release_files(NEW, opts.new.read_bytes())
    portable = opts.work / "portable"
    data = portable / "show23-pc"
    versions = data / "versions"
    write_files(versions / OLD, old_files)
    shutil.copyfile(opts.bootstrap, portable / "show23-pc.exe")
    properties = data / "user/properties.json"
    baseline_settings = {"launcher-update-channel": "beta", "launcher-auto-shortcuts": "false",
                         "fixture-sentinel": "untouched"}
    write_files(data, {"portable.marker": b"", "fixture-mode": NEW.encode(),
                       "user/properties.json": json.dumps(baseline_settings).encode()})
    install = opts.work / "synthetic-install"
    write_files(install, {"manifests/runtime.txt": b"0.1.2\n", "data/sentinel.txt": b"player data\n"})
    before_bootstrap = digest((portable / "show23-pc.exe").read_bytes())

    filename = f"show23-pc-{NEW}-win64-portable"
    zip_asset = opts.work / f"{filename}.zip"
    with ZipFile(zip_asset, "w", ZIP_DEFLATED) as package:
        prefix = f"{filename}/show23-pc/versions/{NEW}/"
        for name, contents in new_files.items():
            package.writestr(prefix + name, contents)
        # This belongs to fresh ZIP extraction only and must not appear in either live root.
        package.writestr(f"{filename}/manual/outside-version.cmd", b"@echo ignored\r\n")
    payload = zip_asset.read_bytes()
    manifest = {
        "schema": 1, "part": "launcher", "version": NEW, "channel": "beta", "min_launcher": OLD,
        "files": [{"path": zip_asset.name, "size": len(payload), "sha256": digest(payload)}],
        "compatibility": {part: "*" for part in ("emulator", "server", "profile")},
    }
    assets = {zip_asset.name: payload, "launcher.json": json.dumps(manifest).encode(),
              "SHA256SUMS.txt": f"{digest(payload)}  {zip_asset.name}\n".encode()}
    connections = []
    release_path = f"/{REPO}/releases/download/v{NEW}/"
    feed_path = f"/repos/{REPO}/releases?per_page=30"

    class Feed(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            connections.append(self.path)
            if self.path == feed_path:
                origin = f"http://127.0.0.1:{self.server.server_port}"
                release = {
                    "tag_name": "v" + NEW, "prerelease": True, "draft": False,
                    "assets": [{"name": name, "size": len(content),
                                "browser_download_url": origin + release_path + name}
                               for name, content in assets.items()],
                }
                content = json.dumps([release]).encode()
            elif self.path.startswith(release_path) and self.path[len(release_path):] in assets:
                content = assets[self.path[len(release_path):]]
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

    command = [portable / "show23-pc.exe", "--update-fixture"]
    with ThreadingHTTPServer(("127.0.0.1", 0), Feed) as host:
        thread = threading.Thread(target=host.serve_forever, daemon=True)
        thread.start()
        try:
            env = dict(os.environ, SHOW23_FIXTURE_PORT=str(host.server_port))
            result = subprocess.run(command, cwd=portable, env=env, timeout=85)
        finally:
            host.shutdown()
            thread.join(timeout=5)
    error = data / "fixture-error.txt"
    assert result.returncode == 0, (result.returncode, error.read_text() if error.exists() else connections)
    pointer = (versions / "pointer.txt").read_text(encoding="ascii")
    assert f"current {NEW}\nprevious {OLD}\ntrial -\nstarts 0\n" in pointer
    for version, files in ((OLD, old_files), (NEW, new_files)):
        assert (data / f"fixture-confirmed-{version}.txt").read_text() == version
        for name, contents in files.items():
            assert (versions / version / name).read_bytes() == contents, name
    assert json.loads((versions / f"{NEW}.launcher.json").read_bytes()) == manifest
    assert not (portable / "manual/outside-version.cmd").exists()
    assert not (versions / NEW / "manual/outside-version.cmd").exists()
    assert not (versions / "request.txt").exists()
    assert not list(versions.glob(".download-*")) and not list(versions.glob("*.partial"))
    assert digest((portable / "show23-pc.exe").read_bytes()) == before_bootstrap
    assert (install / "data/sentinel.txt").read_bytes() == b"player data\n"
    after_settings = json.loads(properties.read_bytes())
    assert all(after_settings[key] == value for key, value in baseline_settings.items())
    assert connections == [feed_path, release_path + "launcher.json",
                           release_path + "SHA256SUMS.txt", release_path + zip_asset.name], connections
    evidence = {"from": OLD, "to": NEW, "requests": connections, "pointer": pointer,
                "readyFolder": str(versions / NEW), "bootstrapSha256": before_bootstrap,
                "oldFilesUnchanged": True, "settingsPreserved": True, "normalRequestWritten": False}
    write_files(opts.work, {"result.json": (json.dumps(evidence, indent=2) + "\n").encode()})
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap", type=Path, required=True)
    parser.add_argument("--old", type=Path, required=True)
    parser.add_argument("--new", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    args.work = args.work.resolve()
    fixture(args)
