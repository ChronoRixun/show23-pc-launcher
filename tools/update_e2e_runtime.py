"""Exercise runtime publication, Repair and contention from Windows processes."""

import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import shutil
import subprocess
import sys
import time
from zipfile import ZipFile


LAUNCHER = "0.1.3-beta.1"
RUNTIME = (Path(__file__).with_name("release-version.txt")).read_text(encoding="ascii").strip()
MARKER = (RUNTIME + "\n").encode("ascii")
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
LOCK_SCRIPT = b'''import ctypes
from ctypes import wintypes
from pathlib import Path
import sys
import time

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                               wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
kernel.CreateFileW.restype = wintypes.HANDLE
handle = kernel.CreateFileW(__file__, 0x80000000, 1, None, 3, 0, None)
if handle == ctypes.c_void_p(-1).value:
    raise ctypes.WinError(ctypes.get_last_error())
Path(sys.argv[1]).write_text("locked")
time.sleep(90)
'''
LOAD_SCRIPT = '''import ctypes
from pathlib import Path
import sys
import time

dll = ctypes.WinDLL(sys.argv[1])
Path(sys.argv[2]).write_text("loaded")
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
        prefix = f"{archive.stem}/show23-pc/runtime/0.1.2/server/python/"
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
        "emulator": {"bb-probe.exe": b"fixture engine, deliberately not executable\n",
                     "MANIFEST.tsv": b"fixture inventory\n", "libc++.dll": b"fixture dependency\n",
                     "bbport.ini": b"fixture config\n", "input.ini": b"fixture bindings\n",
                     "prepare.py": b"# fixture preparer\n", "prep/prepare.py": b"# fixture\n",
                     "prep/link_modules.py": b"# fixture\n", "prep/link_libc.py": b"# fixture\n",
                     "prep/content_profile.py": b"# fixture\n",
                     "support.dll": b"fixture support DLL, deliberately not loadable\n"},
        "server": {"olserver.py": b"# server fixture\n",
                   "manual_mode.py": PROCESS_SCRIPT if play_python else b"# manual fixture\n",
                   "roster_decode.py": b"# roster\n", "card_import.py": b"# cards\n",
                   "tables/ol-messages.csv": b"code,message\n", "replies/fixture.xml": b"<reply />\n"},
        "profile": {"defaults.txt": b"fixture default profile\n"},
    }
    payload["server"].update(play_python or {"python/python.exe": b"fixture python\n"})
    for part, directory in PARTS.items():
        contents = payload[part]
        files.update({f"runtime/{RUNTIME}/{directory}/{name}": data for name, data in contents.items()})
        manifest = {
            "schema": 1, "part": part, "version": RUNTIME, "channel": "stable", "min_launcher": LAUNCHER,
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

    def compare_live(self, extra_runtime=None, expected_manifests=None):
        assert self.marker.read_bytes() == MARKER
        assert (self.marker.parent / "server-state-version.txt").read_bytes() == MARKER
        assert inventory(self.runtime_slot) == (inventory(self.slot / "runtime" / RUNTIME) | (extra_runtime or {}))
        assert inventory(self.manifest_slot) == (expected_manifests if expected_manifests is not None else
                                                  inventory(self.slot / "manifests" / RUNTIME))

    def damage(self):
        assert self.action("stage")["ok"]
        self.compare_live()
        (self.runtime_slot / "emulator/bb-probe.exe").write_bytes(b"damaged active engine\n")

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


def repair_case(ctx, case_id, port, cut, repaired_count):
    f = Fixture(ctx, case_id, port)
    f.damage()
    missing = f.runtime_slot / "tools/profile/defaults.txt"
    missing.unlink()
    healthy = f.runtime_slot / "server/olserver.py"
    original_stat = healthy.stat()
    os.utime(healthy, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns - 48 * 60 * 60 * 1_000_000_000))
    healthy_time = healthy.stat().st_mtime_ns
    extra = "emulator/player-notes.txt"
    write_files(f.runtime_slot, {extra: b"keep in slot"})
    manifest = f.manifest_slot / "emulator.json"
    manifest.write_bytes(manifest.read_bytes() + b"\n")
    before_manifests = inventory(f.manifest_slot)
    exit_code = f.action("repair", cut)
    assert f.marker.read_bytes() == MARKER
    assert f.runtime_slot.is_dir() and f.manifest_slot.is_dir()
    assert inventory(f.manifest_slot) == before_manifests
    assert not list((f.install / "runtime").glob(f".repair-{RUNTIME}-*"))
    exe = "emulator/bb-probe.exe"
    expected_exe = ((f.slot / "runtime" / RUNTIME / exe).read_bytes() if repaired_count else
                    b"damaged active engine\n")
    assert (f.runtime_slot / exe).read_bytes() == expected_exe
    assert missing.exists() is (repaired_count == 2)

    interrupted = inventory(f.install)
    f.action("confirm")
    ready = repaired_count == 2
    state_is(f.action("inspect-runtime"), ready=ready, repairable=not ready)
    assert state_is(f.action("stage"), ready=ready, repairable=not ready)["ok"] is ready
    assert inventory(f.install) == interrupted
    assert state_is(f.action("repair"), ready=True)["ok"]
    f.compare_live({extra: digest(b"keep in slot")}, before_manifests)
    assert healthy.stat().st_mtime_ns == healthy_time
    f.check_protected()
    return {"cut": cut, "crashExit": exit_code, "readyAtCut": ready,
            "replacementsAtCut": repaired_count, "healthyFileAndManifestsPreserved": True}


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
    assert f.action("stage")["ok"]
    assert f.python.is_file()
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
            expected_args = ["play", str(f.install), *named_args]
            assert stub["args"] == expected_args, stub
            for pid in (stub["pid"], stub["child"]):
                handle = kernel.OpenProcess(synchronize, False, pid)
                assert handle, (pid, ctypes.get_last_error())
                children.append(handle)
                assert kernel.WaitForSingleObject(handle, 0) == wait_timeout, pid

            (f.runtime_slot / "emulator/bb-probe.exe").write_bytes(b"damaged after manual start\n")
            original = inventory(f.install)
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


def unleased_case(ctx, case_id, port):
    f = Fixture(ctx, case_id, port, python_files())
    f.damage()
    original = inventory(f.install)
    interpreter = f.runtime_slot / "server/python/python.exe"
    ready = f.top / "direct-runtime-ready"
    script = ("from pathlib import Path; import os, sys, time; "
              "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(90)")
    with (f.top / "direct-runtime.log").open("wb") as log:
        process = subprocess.Popen([interpreter, "-I", "-S", "-c", script, ready],
                                   cwd=f.top, env=f.env, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline and process.poll() is None:
                if ready.is_file() and ready.stat().st_size:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError(f"Direct runtime process failed to start. See {log.name}")
            assert int(ready.read_text()) == process.pid
            state_is(f.action("inspect-runtime"), repairable=True, in_use=True)
            refused = state_is(f.action("repair"), repairable=True, in_use=True)
            assert not refused["ok"] and refused["message"] == "in-use", refused
            assert inventory(f.install) == original
            assert process.poll() is None
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
    state_is(f.action("inspect-runtime"), repairable=True)
    assert state_is(f.action("repair"), ready=True)["ok"]
    f.compare_live()
    f.check_protected()
    return {"repairWhileRunning": refused, "directRuntimePid": process.pid,
            "directRuntimeExit": process.returncode, "repairAfterExit": True}


def locked_case(ctx, case_id, port, *, loaded_dll):
    f = Fixture(ctx, case_id, port)
    f.damage()
    ready = f.top / "external-runtime-ready"
    if loaded_dll:
        target = f.runtime_slot / "emulator/support.dll"
        damaged_bytes = (Path(os.environ["SystemRoot"]) / "System32/version.dll").read_bytes()
        command = [sys.executable, "-I", "-B", "-c", LOAD_SCRIPT, target, ready]
    else:
        target = f.runtime_slot / "server/olserver.py"
        damaged_bytes = LOCK_SCRIPT
        command = [sys.executable, "-I", "-B", target, ready]
    target.write_bytes(damaged_bytes)
    missing = f.runtime_slot / "tools/profile/defaults.txt"
    missing.unlink()

    with (f.top / "external-runtime.log").open("wb") as log:
        process = subprocess.Popen(command, cwd=f.top, env=f.env, stdin=subprocess.DEVNULL,
                                   stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline and process.poll() is None:
                if ready.is_file() and ready.read_text() == ("loaded" if loaded_dll else "locked"):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError(f"External runtime fixture failed to start. See {log.name}")
            state_is(f.action("inspect-runtime"), repairable=True)
            refused = state_is(f.action("repair"), repairable=True)
            assert not refused["ok"] and refused["message"] == "in-use", refused
            exe = "emulator/bb-probe.exe"
            assert (f.runtime_slot / exe).read_bytes() == (f.slot / "runtime" / RUNTIME / exe).read_bytes()
            assert target.read_bytes() == damaged_bytes and not missing.exists()
            assert process.poll() is None
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
    assert state_is(f.action("repair"), ready=True)["ok"]
    f.compare_live()
    f.check_protected()
    return {"blockedFile": target.relative_to(f.runtime_slot).as_posix(), "externalPid": process.pid,
            "repairWhileRunning": refused, "previousFileRepaired": True, "repairAfterExit": True}


def legacy_partial_manifests(ctx, port):
    f = Fixture(ctx, "E15n", port)
    f.damage()
    transaction = f.install / "runtime" / f".repair-{RUNTIME}-1"
    shutil.copytree(f.slot / "runtime", transaction / "new/runtime")
    shutil.copytree(f.slot / "manifests" / RUNTIME, transaction / "new/manifests" / RUNTIME)
    old_runtime = transaction / "old/runtime" / RUNTIME
    old_manifests = transaction / "old/manifests" / RUNTIME
    old_runtime.parent.mkdir(parents=True)
    old_manifests.parent.mkdir(parents=True)
    f.runtime_slot.rename(old_runtime)
    f.manifest_slot.rename(old_manifests)

    # The old repair moved the directories aside. A first file-level attempt
    # restored just the engine manifest before it stopped.
    f.manifest_slot.mkdir()
    manifest = "emulator.json"
    shutil.copyfile(f.slot / "manifests" / RUNTIME / manifest, f.manifest_slot / manifest)
    original_runtime = inventory(old_runtime)
    original_manifests = inventory(old_manifests)
    assert not f.action("stage")["ok"]
    state_is(f.action("inspect-runtime"), repairable=True)
    result = f.action("repair")
    assert result["ok"], result
    state_is(result, ready=True)
    f.compare_live()
    f.check_protected()
    assert inventory(old_runtime) == original_runtime
    assert inventory(old_manifests) == original_manifests
    return {"completed": result, "originalBackupsPreserved": True}


def newer_server_state(ctx, port):
    f = Fixture(ctx, "E15o", port)
    assert f.action("stage")["ok"]
    write_files(f.install, {"manifests/server-state-version.txt": b"99.0.0\n",
                            "data/profiles/default/server/state.json": b'{"state":"preserve"}\n'})
    before = inventory(f.install)
    state = state_is(f.action("inspect-runtime"))
    assert "newer local server" in state["message"], state
    for action in ("stage", "repair"):
        assert not f.action(action)["ok"]
    assert inventory(f.install) == before
    return {"blocked": True, "stateUnchanged": True}


def run(ctx):
    fresh = (
        ("E15a", "runtime-fresh-before-slot", False, False),
        ("E15b", "runtime-fresh-after-slot", True, False),
        ("E15c", "runtime-fresh-before-manifests", True, False),
        ("E15d", "runtime-fresh-after-manifests", True, True),
    )
    repair = (
        ("E15f", "runtime-repair-before-file-1", 0),
        ("E15g", "runtime-repair-after-file-1", 1),
        ("E15h", "runtime-repair-before-file-2", 1),
        ("E15i", "runtime-repair-after-file-2", 2),
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
        for case_id, cut, repaired_count in repair:
            ctx.record(case_id, f"Repair keeps the live slot through {cut}",
                       lambda: repair_case(ctx, case_id, port, cut, repaired_count))
        ctx.record("E15j", "Windows manual Play blocks Repair until its process tree is gone",
                   lambda: manual_case(ctx, "E15j", port))
        ctx.record("E15k", "An unleased process from the active runtime blocks Repair until exit",
                   lambda: unleased_case(ctx, "E15k", port))
        ctx.record("E15l", "A DLL loaded by a process outside the slot blocks its replacement",
                   lambda: locked_case(ctx, "E15l", port, loaded_dll=True))
        ctx.record("E15m", "An external interpreter holding a script open blocks its replacement",
                   lambda: locked_case(ctx, "E15m", port, loaded_dll=False))
        ctx.record("E15n", "File-level Repair completes a partially restored legacy manifest set",
                   lambda: legacy_partial_manifests(ctx, port))
        ctx.record("E15o", "An older bundled server cannot open newer install state",
                   lambda: newer_server_state(ctx, port))
