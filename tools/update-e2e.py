"""Run the disposable versioned updater verification matrix on Windows."""
import argparse
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import threading
import traceback
from zipfile import ZIP_DEFLATED, ZipFile

import update_e2e_boot
import update_e2e_legacy
import update_e2e_runtime
import update_e2e_review
import update_e2e_reuse
import update_e2e_rules
import update_e2e_publication
import update_e2e_history
import update_e2e_window

OLD = "0.1.3-beta.1"
NEW = "0.1.3-beta.2"
STABLE = "0.1.3"
BETA = "0.1.4-beta.1"
REPO = "ChronoRixun/show23-pc-launcher"
SOURCE = Path(__file__).resolve().parents[1]
RESEARCH = SOURCE.parents[1]
PROBES = RESEARCH / "probes/update-e2e"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_files(root, files):
    for name, data in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def json_bytes(value):
    return (json.dumps(value, indent=2) + "\n").encode()


def release_manifest(version, filename, data):
    return {
        "schema": 1, "part": "launcher", "version": version,
        "channel": "beta" if "-" in version else "stable", "min_launcher": OLD,
        "files": [{"path": filename, "size": len(data), "sha256": digest(data)}],
        "compatibility": {part: "*" for part in ("emulator", "server", "profile")},
    }


def release_files(version, exe):
    if b"fixture-mode" not in exe or b"SHOW23_FIXTURE_PORT" not in exe:
        raise ValueError("The launcher must be a gated update-fixture build.")
    files = {
        "show23-pc-launcher.exe": exe,
        "ui/main.html": b"Local update fixture\n",
        "manual/play.cmd": f'@echo off\r\necho {version}:%~1\r\n'.encode(),
        "manifests/launcher.json": json_bytes(release_manifest(version, "show23-pc-launcher.exe", exe)),
        **update_e2e_runtime.runtime_files(),
    }
    channel = "beta" if "-" in version else "stable"
    body = f"show23-pc version 1\nversion {version}\nchannel {channel}\nbootstrap 1\n"
    body += "".join(f"file {digest(data)} {len(data)} {name}\n" for name, data in sorted(files.items()))
    files["version.txt"] = (body + f"end {digest(body.encode())}\n").encode()
    return files


def asset_name(version):
    return f"show23-pc-{version}-win64-native-portable"


def make_asset(version, files):
    top = asset_name(version)
    buffer = BytesIO()
    with ZipFile(buffer, "w", ZIP_DEFLATED) as package:
        for name, data in files.items():
            package.writestr(f"{top}/show23-pc/versions/{version}/{name}", data)
        package.writestr(f"{top}/manual/outside-version.cmd", b"@echo ignored\r\n")
    return buffer.getvalue()


def require_portable_extraction(source, destination):
    with ZipFile(source) as archive:
        for info in archive.infolist():
            parts = PurePosixPath(info.filename).parts
            if len(parts) < 2 or ".." in parts or "\\" in info.filename or ":" in info.filename:
                raise AssertionError(f"Unexpected ZIP path {info.filename}")
            target = destination.joinpath(*parts[1:])
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as infile, target.open("wb") as outfile:
                    shutil.copyfileobj(infile, outfile)


class Matrix:
    def __init__(self, options):
        self.__dict__.update(vars(options))
        self.root = self.work.resolve()
        scopes = [(PROBES / milestone).resolve() for milestone in ("m4", "m5", "m6", "m7")]
        if not any(scope in self.root.parents for scope in (*scopes, RESEARCH / "probes/engine-switch")):
            raise ValueError("Choose a new disposable test directory under probes/engine-switch or an updater test milestone.")
        self.root.mkdir()
        self.results = []

    def record(self, case, expected, callback):
        if self.cases and not any(case == wanted or re.match(re.escape(wanted) + r"[-a-z]", case)
                                  for wanted in self.cases.split(",")):
            return
        row = {"id": case, "expected": expected}
        try:
            row["actual"] = callback()
            row["result"] = "pass"
        except Exception as error:
            row["result"] = "fail"
            row["actual"] = {"error": str(error), "traceback": traceback.format_exc(limit=7)}
        self.results.append(row)
        (self.root / "results.json").write_bytes(json_bytes(self.results))
        print(f"{row['result'].upper():4} {case}: {expected}", flush=True)
        if row["result"] == "fail":
            print("     " + str(row["actual"]["error"])[:700], flush=True)

    def confirm_normal(self, top, version, *, already_current=False):
        return update_e2e_window.confirm(Path(top), version, already_current=already_current)

    def confirm_legacy(self, top):
        return update_e2e_window.confirm(Path(top))


class Copy:
    def __init__(self, ctx, case, initial=OLD, binary=None, shortcuts=False, use_zip=False):
        self.top = ctx.root / "core" / case
        self.data = self.top / "show23-pc"
        self.versions = self.data / "versions"
        self.install = self.top / "fixture install"
        self.pointer = self.versions / "pointer.txt"
        self.properties = self.data / "user/properties.json"
        self.bootstrap = self.top / "show23-pc.exe"
        self.initial = initial
        self.shortcuts = shortcuts
        self.files = release_files(initial, (binary or ctx.old).read_bytes())
        if use_zip:
            require_portable_extraction(BytesIO(make_asset(initial, self.files)), self.top)
        else:
            write_files(self.versions / initial, self.files)
        self.top.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ctx.bootstrap, self.bootstrap)
        self.settings = {"launcher-update-channel": "beta", "launcher-auto-shortcuts": str(not not shortcuts).lower(),
                         "fixture-sentinel": "settings preserved", "show23-install": str(self.install)}
        write_files(self.data, {"portable.marker": b"", "fixture-mode": initial.encode(),
                               "user/properties.json": json_bytes(self.settings)})
        self.sentinels = {"fixture install/data/user/save.dat": b"player save\n",
                          "fixture install/data/logs/session.log": b"prior session\n",
                          "manual/player-note.txt": b"manual notes\n", "LICENSES/player-note.txt": b"licence notes\n"}
        write_files(self.top, self.sentinels)
        self.bootstrap_bytes = self.bootstrap.read_bytes()

    def stage_target(self, target):
        (self.data / "fixture-mode").write_text(target, encoding="ascii")

    def start(self, *, port=1, action="confirm", cut=None, expect=0, **extra):
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith("SHOW23_FIXTURE_")}
        env.update(SHOW23_FIXTURE_PORT=str(port), SHOW23_FIXTURE_ACTION=action, **extra)
        if self.shortcuts:
            env["SHOW23_FIXTURE_SHORTCUTS"] = "1"
        if cut:
            env["SHOW23_FIXTURE_KILL_AT"] = cut
        proc = subprocess.run([self.bootstrap, "--update-fixture"], cwd=self.top, env=env,
                              capture_output=True, timeout=70)
        detail = self.data / "fixture-error.txt"
        assert proc.returncode == expect, (proc.returncode, detail.read_text() if detail.exists() else proc.stderr)
        return proc.returncode

    def state(self):
        return self.pointer.read_text(encoding="ascii")

    def check(self, *, except_settings=()):
        assert self.bootstrap.read_bytes() == self.bootstrap_bytes
        for name, data in self.sentinels.items():
            assert (self.top / name).read_bytes() == data, name
        properties = json.loads(self.properties.read_bytes())
        assert all(properties[k] == value for k, value in self.settings.items() if k not in except_settings)
        return {"sentinels": len(self.sentinels), "preservedProperties": sorted(set(self.settings) - set(except_settings))}


@contextmanager
def feed(version, files, *, before_listing=None):
    name = asset_name(version) + ".zip"
    payload = make_asset(version, files)
    manifest = release_manifest(version, name, payload)
    assets = {name: payload, "launcher.json": json_bytes(manifest),
              "SHA256SUMS.txt": f"{digest(payload)}  {name}\n".encode()}
    requests = []
    release_path = f"/{REPO}/releases/download/v{version}/"
    prefix = f"/repos/{REPO}/releases"

    class Feed(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            requests.append(self.path)
            if self.path in (prefix + "?per_page=30", prefix + "/latest"):
                if before_listing:
                    before_listing()
                origin = f"http://127.0.0.1:{self.server.server_port}"
                release = {"tag_name": "v" + version, "prerelease": "-" in version, "draft": False,
                           "assets": [{"name": leaf, "size": len(data),
                                       "browser_download_url": origin + release_path + leaf}
                                      for leaf, data in assets.items()]}
                contents = json_bytes(release if self.path.endswith("/latest") else [release])
            elif self.path.startswith(release_path) and self.path[len(release_path):] in assets:
                contents = assets[self.path[len(release_path):]]
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(contents)))
            self.end_headers()
            try:
                self.wfile.write(contents)
            except (ConnectionAbortedError, BrokenPipeError, ConnectionResetError):
                pass

    with ThreadingHTTPServer(("127.0.0.1", 0), Feed) as host:
        thread = threading.Thread(target=host.serve_forever, daemon=True)
        thread.start()
        try:
            yield host.server_port, requests, manifest
        finally:
            host.shutdown()
            thread.join(timeout=5)


def transient(copy):
    return sorted(path.name for path in copy.versions.iterdir()
                  if path.name.startswith(".download-") or path.name.endswith(".partial"))


def expect_current(copy, current, previous=None):
    pointer = copy.state()
    assert f"current {current}\n" in pointer
    if previous is not None:
        assert f"previous {previous}\n" in pointer
    assert "trial -\nstarts 0\n" in pointer
    return pointer


def fresh(ctx):
    copy = Copy(ctx, "E1", use_zip=True)
    assert not copy.pointer.exists()
    copy.start(action="stage", SHOW23_FIXTURE_INSTALL=str(copy.install))
    actual = json.loads((copy.data / "fixture-runtime-result.json").read_bytes())
    assert actual["ok"] and actual["ready"]
    assert not (copy.versions / "notice.txt").exists()
    for name, data in update_e2e_runtime.runtime_files().items():
        assert (copy.install / name).read_bytes() == data, name
    return {"pointer": expect_current(copy, OLD, "-"), "runtime": actual, **copy.check()}


def update(ctx, case="E2", action="update", shortcuts=False):
    copy = Copy(ctx, case, shortcuts=shortcuts)
    write_files(copy.install, update_e2e_runtime.runtime_files())
    copy.start()
    before = read_links(copy) if shortcuts else {}
    copy.stage_target(NEW)
    files = release_files(NEW, ctx.new.read_bytes())
    with feed(NEW, files) as (port, requests, manifest):
        copy.start(port=port, action=action, SHOW23_FIXTURE_INSTALL=str(copy.install))
    pointer = expect_current(copy, NEW, OLD)
    assert len(requests) == 4 and requests[0].endswith("?per_page=30") and requests[-1].endswith(".zip"), requests
    assert json.loads((copy.versions / f"{NEW}.launcher.json").read_bytes()) == manifest
    for tag, contents in ((OLD, copy.files), (NEW, files)):
        assert all((copy.versions / tag / name).read_bytes() == data for name, data in contents.items())
    assert not transient(copy) and not (copy.versions / "request.txt").exists()
    assert not (copy.top / "manual/outside-version.cmd").exists()
    assert not (copy.versions / NEW / "manual/outside-version.cmd").exists()
    for name, data in update_e2e_runtime.runtime_files().items():
        assert (copy.install / name).read_bytes() == data, name
    evidence = {"pointer": pointer, "requests": requests, "completeBytesPreserved": True, **copy.check()}
    if action == "lease-update":
        assert (copy.data / "fixture-restart-blocked.txt").read_text() == "busy"
        evidence["whileSharedLease"] = "Update completed, restart rejected with busy; activated after release."
    if shortcuts:
        after = read_links(copy)
        assert before == after
        evidence["linksBeforeAndAfter"] = list(after.values())
    return evidence


def updater_cut(ctx, cut, committed):
    copy = Copy(ctx, "E5-" + cut)
    copy.start()
    copy.stage_target(NEW)
    new = release_files(NEW, ctx.new.read_bytes())
    with feed(NEW, new) as (port, requests, _):
        copy.start(port=port, action="update", cut=cut, expect=111)
        left = transient(copy)
        assert (copy.versions / NEW).is_dir() == committed
        assert left and any(name.startswith(".download-") for name in left)
        if cut in ("U3-extract", "U4-verified", "U5-before-rename"):
            assert NEW + ".partial" in left
        before = expect_current(copy, OLD)
        copy.start()
        recovered = expect_current(copy, NEW if committed else OLD)
        assert not transient(copy)
        if not committed:
            copy.start(port=port, action="update")
            expect_current(copy, NEW, OLD)
    return {"interrupted": left, "before": before, "recovered": recovered, "requests": requests, **copy.check()}


def channel_roundtrip(ctx, redownload):
    name = "E8-download" if redownload else "E8-reuse"
    copy = Copy(ctx, name, STABLE, ctx.stable)
    copy.start()
    copy.stage_target(BETA)
    with feed(BETA, release_files(BETA, ctx.next_beta.read_bytes())) as (port, beta_requests, _):
        copy.start(port=port, action="update", SHOW23_FIXTURE_CHANNEL="beta")
    expect_current(copy, BETA, STABLE)
    stray = "0.1.4-beta.0"
    # Retain a Beta other than previous, as would happen after several Beta updates.
    write_files(copy.versions / stray, release_files(stray, ctx.next_beta.read_bytes()))
    if redownload:
        victim = (copy.versions / STABLE).resolve()
        assert victim.parent == copy.versions.resolve()
        shutil.rmtree(victim)
    copy.stage_target(STABLE)
    with feed(STABLE, copy.files) as (port, stable_requests, _):
        copy.start(port=port, action="switch")
    pointer = expect_current(copy, STABLE, BETA)
    assert len(beta_requests) == 4
    assert stable_requests[0].endswith("/latest")
    assert len(stable_requests) == (4 if redownload else 1), stable_requests
    request = next(line[13:] for line in pointer.splitlines() if line.startswith("last-request "))
    assert len(request) == 16
    assert not (copy.versions / "request.txt").exists()
    copy.start()
    assert expect_current(copy, STABLE, BETA) == pointer
    assert (copy.versions / stray).is_dir()
    assert (copy.versions / "channel.txt").read_bytes() == b"show23-pc channel 1\nstable\n"
    return {"betaRequests": beta_requests, "stableRequests": stable_requests, "consumedRequest": request,
            "reopenedOnStable": True, "pointer": pointer, **copy.check()}


def manual_forwarders(ctx):
    copy = Copy(ctx, "E13")
    for source in (SOURCE / "tools/manual-forwarders").glob("*"):
        shutil.copyfile(source, copy.top / "manual" / source.name)

    def pick():
        proc = subprocess.run(r'cmd.exe /d /c manual\play.cmd "input with spaces"',
                              cwd=copy.top, text=True, capture_output=True, timeout=25)
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        return proc.stdout.strip()

    assert not copy.pointer.exists()
    fresh = pick()
    assert fresh == f"{OLD}:input with spaces" and not copy.pointer.exists()
    copy.start()
    # Present an arrival and invoke production --prepare, which may select it as trial,
    # while the manual command must still dispatch to the confirmed current.
    write_files(copy.versions / NEW, release_files(NEW, ctx.new.read_bytes()))
    prepared = subprocess.run([copy.bootstrap, "--prepare"], cwd=copy.top, text=True, capture_output=True, timeout=20)
    assert prepared.returncode == 0 and prepared.stdout.strip() == OLD
    assert f"trial {NEW}\n" in copy.state()
    during = pick()
    assert during == fresh
    copy.stage_target(NEW)
    copy.start()
    after = pick()
    assert after == f"{NEW}:input with spaces"
    return {"fresh": fresh, "duringTrial": during, "after": after, "pointer": copy.state(), **copy.check()}


def read_links(copy):
    root = copy.data / "fixture-shortcuts"
    links = list(sorted(root.rglob("*.lnk")))
    assert len(links) == 2, links
    shell = "$obj = New-Object -ComObject WScript.Shell; $obj.CreateShortcut($env:SHOW23_FIXTURE_LINK).TargetPath"
    result = {}
    for link in links:
        env = dict(os.environ, SHOW23_FIXTURE_LINK=str(link))
        target = subprocess.check_output(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", shell],
                                         env=env, text=True, timeout=10)
        assert Path(target.strip()) == copy.bootstrap
        result[link.relative_to(root).as_posix()] = target.strip()
    return result


def visible_start(ctx):
    assert ctx.normal_zip and ctx.normal_zip.is_file()
    top = ctx.root / "visible"
    require_portable_extraction(ctx.normal_zip, top)
    store = top / "show23-pc"
    write_files(store, {"user/properties.json": json_bytes({"launcher-auto-shortcuts": "false",
                                                           "launcher-offline-mode": "true",
                                                           "fixture-sentinel": "visible"})})
    prepared = subprocess.run([top / "show23-pc.exe", "--prepare"], cwd=top, capture_output=True, text=True, timeout=25)
    assert prepared.returncode == 0 and prepared.stdout.strip() == OLD
    pointer = store / "versions/pointer.txt"
    assert f"current -\nprevious -\ntrial {OLD}\nstarts 0\n" in pointer.read_text()
    assert not (store / "versions/notice.txt").exists()
    capture = ctx.root / "cef-confirmation.png"
    if not ctx.capture_tool or not ctx.capture_tool.is_file():
        raise ValueError("Supply the configured window capture script with --capture-tool.")
    return update_e2e_window.confirm(top, OLD, capture, ctx.capture_tool, second=True)


def run(ctx):
    ctx.record("E1", "Fresh extraction confirms a trial and stages the bundled runtime.", lambda: fresh(ctx))
    ctx.record("E2", "An in-app update confirms Beta 2, preserving earlier files and the installed runtime.",
               lambda: update(ctx))
    for cut, committed in (("U2-download", False), ("U2-downloaded", False), ("U3-extract", False),
                           ("U4-verified", False), ("U5-before-rename", False), ("U5-after-rename", True),
                           ("U6-before-cleanup", True)):
        ctx.record("E5-" + cut, "Restart recovers the verified arrival or keeps current and cleans artifacts.",
                   lambda cut=cut, committed=committed: updater_cut(ctx, cut, committed))
    update_e2e_boot.run(ctx)
    update_e2e_review.run(ctx)
    update_e2e_rules.run(ctx, Copy, release_files, STABLE, BETA)
    update_e2e_history.run(ctx)
    update_e2e_publication.run(ctx, Copy, release_files, feed, expect_current, transient, write_files, OLD, NEW, STABLE, BETA)
    for case, same, competing in (("E16-mismatch", False, False), ("E16-identical", True, False),
                                  ("E16-competing", True, True)):
        ctx.record(case, "A downloaded receipt only identifies identical verified bytes; the selected bytes start.",
                   lambda case=case, same=same, competing=competing: update_e2e_reuse.same_tag(
                       ctx, case, same, Copy, release_files, feed, expect_current, transient, write_files,
                       NEW, OLD, competing=competing))
    ctx.record("E8-reuse", "Stable to Beta and back to the verified Stable slot without a download.",
               lambda: channel_roundtrip(ctx, False))
    ctx.record("E8-download", "Stable to Beta and back to Stable by explicit download after its slot is removed.",
               lambda: channel_roundtrip(ctx, True))
    if ctx.archived_legacy:
        update_e2e_legacy.run(ctx)
    ctx.record("E12", "A held runtime lease allows update publication and blocks Restart until released.",
               lambda: update(ctx, "E12", action="lease-update"))
    ctx.record("E13", "Windows manual forwarding selects trial when fresh, then current during and after trial.",
               lambda: manual_forwarders(ctx))
    ctx.record("E14-shortcuts", "Actual shortcut targets keep the top-level bootstrap across an update.",
               lambda: update(ctx, "E14-shortcuts", shortcuts=True))
    ctx.record("E14-window", "Normal CEF confirmation, direct-start handoff, foreground restoration and capture.",
               lambda: visible_start(ctx))
    update_e2e_runtime.run(ctx)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("bootstrap", "old", "new", "stable", "next-beta", "work"):
        parser.add_argument("--" + name, type=lambda path: Path(path).resolve(), required=True)
    parser.add_argument("--normal-zip", type=lambda path: Path(path).resolve())
    parser.add_argument("--capture-tool", type=lambda path: Path(path).resolve())
    parser.add_argument("--archived-legacy", action="store_true", help="Run historical cases with archived 0.1.x inputs")
    parser.add_argument("--legacy-runtime-probe", type=lambda path: Path(path).resolve())
    parser.add_argument("--normal-runtime-probe", type=lambda path: Path(path).resolve())
    parser.add_argument("--cases", help="Comma-separated case IDs or case families.")
    parser.add_argument("--legacy-zip", type=lambda path: Path(path).resolve(),
                        default=RESEARCH / "release/published-0.1.0/show23-pc-0.1.0-win64-portable.zip")
    parser.add_argument("--legacy-exe", type=lambda path: Path(path).resolve(),
                        default=PROBES / "m2/legacy-headless.exe")
    parser.add_argument("--legacy-source", type=lambda path: Path(path).resolve(),
                        default=PROBES / "m2/legacy-launcher-update.cpp")
    context = Matrix(parser.parse_args())
    run(context)
    failures = sum(row["result"] != "pass" for row in context.results)
    print(f"{len(context.results)} cases, {failures} failed")
    sys.exit(1 if failures or not context.results else 0)
