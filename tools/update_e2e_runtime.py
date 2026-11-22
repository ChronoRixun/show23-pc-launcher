"""Exercise real runtime publication, Repair and the Windows manual Play lease."""

import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
from zipfile import ZipFile


LAUNCHER = "0.1.3-beta.1"
RUNTIME = "0.1.2"
MARKER = b"0.1.2\n"
CRASH_EXIT = 111
PARTS = {"emulator": "emulator", "server": "server", "profile": "tools/profile"}
PROTECTED = {
    "data/profiles/default/user/save/slot.sav": b"fixture save\n",
    "data/logs/previous-play.log": b"retained play log\n",
    "data/games/CUSA35521/game.json": b'{"fixture":true}\n',
    "manifests/game-ready.txt": b"CUSA35521\n",
    "manifests/custom.txt": b"unrelated install manifest\n",
}
PROCESS_SCRIPT = b'''import json
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(os.environ["SHOW23_FIXTURE_MANUAL_STATE"])
if sys.argv[1:] == ["--fixture-grandchild"]:
    (root / "grandchild-ready").write_text(str(os.getpid()))
else:
    child = subprocess.Popen([sys.executable, "-B", __file__, "--fixture-grandchild"],
                            creationflags=subprocess.CREATE_NO_WINDOW)
    temporary = root / "manual-started.tmp"
    temporary.write_text(json.dumps({"pid": os.getpid(), "child": child.pid, "args": sys.argv[1:]}))
    temporary.replace(root / "manual-started.json")
time.sleep(90)
'''


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_files(root, files):
    for name, data in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def inventory(root):
    return {item.relative_to(root).as_posix(): "directory" if item.is_dir() else digest(item.read_bytes())
            for item in root.rglob("*")}


def python_files():
    archive = Path(__file__).resolve().parents[1] / "dist/show23-pc-0.1.2-win64-portable.zip"
    if archive.is_file():
        prefix = f"{archive.stem}/show23-pc/runtime/{RUNTIME}/server/python/"
        runtime_file = re.compile(r"python(?:\.exe|3\.dll|3\d+\.(?:dll|zip|_pth))|vcruntime140(?:_1)?\.dll")
        files = {}
        with ZipFile(archive) as package:
            for entry in package.namelist():
                name = entry.removeprefix(prefix)
                if name != entry and runtime_file.fullmatch(name):
                    files["python/" + name] = package.read(entry)
        if "python/python.exe" in files:
            return files

    # A normal CPython install is also usable. Its startup path is pinned to the
    # local standard library and vendor DLLs. No site packages are loaded.
    base = Path(sys.base_prefix)
    files = {"python/python.exe": (base / "python.exe").read_bytes()}
    for pattern in ("python3*.dll", "vcruntime140*.dll"):
        files.update({"python/" + path.name: path.read_bytes() for path in base.glob(pattern)})
    abi = f"python{sys.version_info.major}{sys.version_info.minor}"
    files[f"python/{abi}._pth"] = f"{base / 'Lib'}\n{base / 'DLLs'}\n".encode("utf-8")
    return files


def runtime_files(play_python=None):
    files = {"manifests/runtime.txt": MARKER}
    payload = {
        "emulator": {"shadPS4.exe": b"fixture emulator, deliberately not executable\n"},
        "server": {"olserver.py": PROCESS_SCRIPT if play_python else b"# frozen fixture\n",
                   "roster_decode.py": b"# roster\n", "card_import.py": b"# cards\n",
                   "tables/ol-messages.csv": b"code,message\n", "replies/fixture.xml": b"<reply />\n"},
        "profile": {"defaults.txt": b"fixture default profile\n"},
    }
    payload["server"].update(play_python or {"python/python.exe": b"fixture python\n"})
    for part, directory in PARTS.items():
        contents = payload[part]
        files.update({f"runtime/{RUNTIME}/{directory}/{name}": data for name, data in contents.items()})
        manifest = {
            "schema": 1, "part": part, "version": RUNTIME, "channel": "stable", "min_launcher": RUNTIME,
            "files": [{"path": name, "size": len(data), "sha256": digest(data)}
                      for name, data in sorted(contents.items())],
            "compatibility": {other: "*" for other in ("launcher", *PARTS) if other != part},
        }
        files[f"manifests/{RUNTIME}/{part}.json"] = json.dumps(manifest).encode()
    return files


class Fixture:
    def __init__(self, ctx, case_id, port, play_python=None):
        self.top = ctx.root / "runtime cases" / case_id
        self.data = self.top / "show23-pc"
        self.slot = self.data / "versions" / LAUNCHER
        self.install = self.top / "fixture install with spaces"
        self.runtime_slot = self.install / "runtime" / RUNTIME
        self.manifest_slot = self.install / "manifests" / RUNTIME
        self.marker = self.install / "manifests/runtime.txt"
        self.result = self.data / "fixture-runtime-result.json"
        self.python = self.slot / "runtime" / RUNTIME / "server/python/python.exe"
        self.wrapper = self.slot / "show23-pc-launcher.exe"
        self.bootstrap = self.top / "show23-pc.exe"
        env = {name: value for name, value in os.environ.items() if not name.upper().startswith("SHOW23_FIXTURE_")}
        self.env = dict(env, SHOW23_FIXTURE_PORT=str(port), SHOW23_FIXTURE_INSTALL=str(self.install))

        self.top.mkdir(parents=True)
        launcher = {"show23-pc-launcher.exe": ctx.old.read_bytes(),
                    "ui/main.html": b"Runtime fixture\n", **runtime_files(play_python)}
        body = (f"show23-pc version 1\nversion {LAUNCHER}\nchannel beta\nbootstrap 1\n"
                + "".join(f"file {digest(data)} {len(data)} {name}\n" for name, data in sorted(launcher.items()))).encode()
        launcher["version.txt"] = body + f"end {digest(body)}\n".encode()
        write_files(self.slot, launcher)
        write_files(self.top, {"show23-pc.exe": ctx.bootstrap.read_bytes()})
        write_files(self.data, {"portable.marker": b"", "fixture-mode": LAUNCHER.encode()})
        write_files(self.install, PROTECTED)
        self.bundle_before = inventory(self.slot)

    def action(self, action, cut=None):
        self.result.unlink(missing_ok=True)
        env = dict(self.env, SHOW23_FIXTURE_ACTION=action)
        if cut:
            env["SHOW23_FIXTURE_KILL_AT"] = cut
        process = subprocess.run([self.bootstrap, "--update-fixture"], cwd=self.top, env=env, timeout=30)
        if cut:
            assert process.returncode == CRASH_EXIT and not self.result.exists(), (cut, process.returncode, self.result)
            return process.returncode
        assert process.returncode == 0, (action, process.returncode, self.data / "versions/bootstrap.log")
        if action == "confirm":
            return
        state = json.loads(self.result.read_bytes())
        assert set(state) >= {"ok", "ready", "fresh", "repairable", "inUse", "message"}, state
        return state

    def compare_live(self):
        assert self.marker.read_bytes() == MARKER
        assert inventory(self.runtime_slot) == inventory(self.slot / "runtime" / RUNTIME)
        assert inventory(self.manifest_slot) == inventory(self.slot / "manifests" / RUNTIME)

    def damage(self):
        assert self.action("stage")["ok"]
        self.compare_live()
        (self.runtime_slot / "emulator/shadPS4.exe").write_bytes(b"damaged active emulator\n")
        manifest = self.manifest_slot / "emulator.json"
        manifest.write_bytes(manifest.read_bytes() + b"\n")

    def check_protected(self):
        assert all((self.install / name).read_bytes() == payload for name, payload in PROTECTED.items())
        assert inventory(self.slot) == self.bundle_before


def state_is(state, *, ready=False, fresh=False, repairable=False, in_use=False):
    assert (state["ready"], state["fresh"], state["repairable"], state["inUse"]) == (
        ready, fresh, repairable, in_use), state
    return state


def fresh_case(ctx, case_id, port, cut, slot_published, manifests_published):
    f = Fixture(ctx, case_id, port)
    state_is(f.action("inspect-runtime"), fresh=True)
    exit_code = f.action("stage", cut)
    assert not f.marker.exists()
    (staged,) = (f.install / "runtime").glob(".bundled-*")
    assert (staged / ".fresh-stage").read_bytes() == b"show23-fresh-runtime-v1\n" + MARKER
    assert f.runtime_slot.is_dir() is slot_published
    assert f.manifest_slot.is_dir() is manifests_published
    assert (staged / "runtime" / RUNTIME).is_dir() is not slot_published
    assert (staged / "manifests" / RUNTIME).is_dir() is not manifests_published
    interrupted = inventory(f.install)
    state_is(f.action("inspect-runtime"), fresh=True)
    assert inventory(f.install) == interrupted
    assert state_is(f.action("stage"), ready=True)["ok"]
    assert not list((f.install / "runtime").glob(".bundled-*"))
    f.compare_live()
    f.check_protected()
    return {"cut": cut, "crashExit": exit_code, "slotPublishedAtCut": slot_published,
            "manifestsPublishedAtCut": manifests_published, "markerPublishedOnRetry": True}


def damaged_case(ctx, case_id, port):
    f = Fixture(ctx, case_id, port)
    f.damage()
    original = inventory(f.install)
    f.action("confirm")
    status = state_is(f.action("inspect-runtime"), repairable=True)
    assert "failed verification" in status["message"], status
    assert not state_is(f.action("stage"), repairable=True)["ok"]
    assert inventory(f.install) == original
    f.check_protected()
    return {"homeRuntimeStatus": status, "damagedFilesUnchanged": True}


def repair_case(ctx, case_id, port, cut, step):
    f = Fixture(ctx, case_id, port)
    f.damage()
    before_runtime = inventory(f.runtime_slot)
    before_manifests = inventory(f.manifest_slot)
    exit_code = f.action("repair", cut)
    assert f.marker.read_bytes() == MARKER
    (transaction,) = (f.install / "runtime").glob(f".repair-{RUNTIME}-*")
    saved_runtime = transaction / "old/runtime" / RUNTIME
    saved_manifests = transaction / "old/manifests" / RUNTIME
    assert inventory(saved_runtime) == before_runtime
    assert saved_manifests.is_dir() is (step >= 2)
    assert f.runtime_slot.is_dir() is (step == 4)
    assert f.manifest_slot.is_dir() is (step != 2)
    assert (transaction / "new/runtime" / RUNTIME).is_dir() is (step < 4)
    assert (transaction / "new/manifests" / RUNTIME).is_dir() is (step < 3)

    interrupted = inventory(f.install)
    f.action("confirm")
    state_is(f.action("inspect-runtime"), ready=step == 4, repairable=step < 4)
    assert state_is(f.action("stage"), ready=step == 4, repairable=step < 4)["ok"] is (step == 4)
    assert inventory(f.install) == interrupted
    assert state_is(f.action("repair"), ready=True)["ok"]
    f.compare_live()
    assert inventory(saved_runtime) == before_runtime
    assert inventory(saved_manifests) == before_manifests
    assert not (transaction / "new/runtime" / RUNTIME).exists()
    assert not (transaction / "new/manifests" / RUNTIME).exists()
    assert list((f.install / "runtime").glob(f".repair-{RUNTIME}-*")) == [transaction]
    f.check_protected()
    return {"cut": cut, "crashExit": exit_code, "readyAtCut": step == 4,
            "requiredExplicitRepair": step < 4, "backupPreserved": True}


def observe_manual(wrapper, state_dir):
    started = state_dir / "manual-started.json"
    descendant = state_dir / "grandchild-ready"
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline and wrapper.poll() is None:
        if started.is_file() and descendant.is_file() and descendant.stat().st_size:
            state = json.loads(started.read_bytes())
            assert int(descendant.read_text()) == state["child"], state
            return state
        time.sleep(0.05)
    raise AssertionError(f"Manual Play stub failed to start (wrapper exit {wrapper.poll()}). See {state_dir / 'manual.log'}")


def manual_case(ctx, case_id, port):
    f = Fixture(ctx, case_id, port, python_files())
    f.damage()
    assert f.python.is_file()
    original = inventory(f.install)
    state_dir = f.top / "manual state"
    state_dir.mkdir()
    manual_env = dict(f.env, SHOW23_FIXTURE_MANUAL_STATE=str(state_dir))
    named_args = ["--name", 'Player "One"']

    # Open wait handles before killing the wrapper, which pins the relevant process
    # identities even if their PIDs could be recycled during process-tree shutdown.
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.restype = wintypes.BOOL
    synchronize = 0x00100000
    wait_timeout = 258
    wait_signaled = 0
    children = []
    with (state_dir / "manual.log").open("wb") as log:
        wrapper = subprocess.Popen([f.wrapper, "--manual-play", f.install, *named_args], cwd=f.top, env=manual_env,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        try:
            stub = observe_manual(wrapper, state_dir)
            expected_args = ["play", "--install", str(f.install), *named_args, "--emulator",
                             str(f.slot / "runtime" / RUNTIME / "emulator/shadPS4.exe")]
            assert stub["args"] == expected_args, stub
            for pid in (stub["pid"], stub["child"]):
                handle = kernel.OpenProcess(synchronize, False, pid)
                assert handle, (pid, ctypes.get_last_error())
                children.append(handle)
                assert kernel.WaitForSingleObject(handle, 0) == wait_timeout, pid

            refused = state_is(f.action("repair"), repairable=True, in_use=True)
            assert not refused["ok"] and refused["message"] == "in-use", refused
            assert inventory(f.install) == original
            assert wrapper.poll() is None, "Manual Play ended before forced exit"
        finally:
            try:
                if wrapper.poll() is None:
                    wrapper.kill()
                wrapper.wait(timeout=10)
                for handle in children:
                    assert kernel.WaitForSingleObject(handle, 10000) == wait_signaled, "Manual Play child survived"
            finally:
                for handle in children:
                    kernel.CloseHandle(handle)

    assert len(children) == 2
    state_is(f.action("inspect-runtime"), repairable=True)
    assert state_is(f.action("repair"), ready=True)["ok"]
    f.compare_live()
    f.check_protected()
    return {"repairWhilePlaying": refused, "wrapperPid": wrapper.pid,
            "pythonPid": stub["pid"], "grandchildPid": stub["child"],
            "wrapperExit": wrapper.returncode, "processTreeExited": True, "repairAfterKill": True}


def run(ctx):
    fresh = (
        ("E15a", "runtime-fresh-before-slot", False, False),
        ("E15b", "runtime-fresh-after-slot", True, False),
        ("E15c", "runtime-fresh-before-manifests", True, False),
        ("E15d", "runtime-fresh-after-manifests", True, True),
    )
    repair = (
        ("E15f", "runtime-repair-after-save-slot", 1),
        ("E15g", "runtime-repair-after-save-manifests", 2),
        ("E15h", "runtime-repair-after-publish-manifests", 3),
        ("E15i", "runtime-repair-after-publish-slot", 4),
    )
    # The guarded fixture requires a valid loopback origin. Runtime actions do not
    # need HTTP, so keep a port bound without serving any release assets.
    with socket.socket() as loopback:
        loopback.bind(("127.0.0.1", 0))
        port = loopback.getsockname()[1]
        for case_id, cut, slot, manifests in fresh:
            ctx.record(case_id, f"Fresh staging completes after {cut}",
                       lambda: fresh_case(ctx, case_id, port, cut, slot, manifests))
        ctx.record("E15e", "A damaged runtime stays in place and reports Repair eligibility",
                   lambda: damaged_case(ctx, "E15e", port))
        for case_id, cut, step in repair:
            ctx.record(case_id, f"Repair preserves originals after {cut}",
                       lambda: repair_case(ctx, case_id, port, cut, step))
        ctx.record("E15j", "Windows manual Play blocks Repair until its process tree is gone",
                   lambda: manual_case(ctx, "E15j", port))
