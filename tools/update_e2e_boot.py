"""Disposable bootstrap recovery scenarios for update-e2e.py."""

from contextlib import contextmanager
import csv
import ctypes
from ctypes import wintypes
import hashlib
import os
import shutil
import stat
import subprocess
import time


OLD = "0.1.3-beta.1"
NEW = "0.1.3-beta.2"
REQUEST_ID = "a1389f39d3b84ece"
STALE_ID = "9f914a2f32cb60dd"
BOOTSTRAP_KILL = 111


def sealed(body):
    contents = body.encode("ascii")
    return contents + f"end {hashlib.sha256(contents).hexdigest()}\n".encode("ascii")


def pointer(current="-", previous="-", trial="-", starts=0, request="-"):
    return sealed(f"show23-pc pointer 1\ncurrent {current}\nprevious {previous}\n"
                  f"trial {trial}\nstarts {starts}\nlast-request {request}\n")


class Fixture:
    def __init__(self, ctx, name):
        self.root = ctx.root / "bootstrap" / name
        self.top = self.root / "portable"
        self.data = self.top / "show23-pc"
        self.versions = self.data / "versions"
        self.primary = self.versions / "pointer.txt"
        self.backup = self.versions / "pointer.bak"
        self.request = self.versions / "request.txt"
        self.log = self.versions / "bootstrap.log"
        self.executable = self.top / "show23-pc.exe"
        self.sources = {OLD: ctx.old, NEW: ctx.new}
        self.versions.mkdir(parents=True)
        shutil.copyfile(ctx.bootstrap, self.executable)
        self.write(self.data / "portable.marker", b"")
        self.write(self.data / "fixture-mode", NEW.encode("ascii"))
        self.sentinels = {
            self.data / "user/properties.json": b'{"launcher-update-channel":"beta","fixture":"sentinel"}\n',
            self.data / "user/launcher.log": b"existing launcher log\r\n",
            self.root / "install/data/saves/update-fixture.sav": b"existing save\x00\x01",
            self.top / "manual/player.cmd": b"rem player-added script\r\n",
            self.top / "LICENSES/player.txt": b"player-added license file\n",
        }
        for path, contents in self.sentinels.items():
            self.write(path, contents)

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def stage(self, *versions, broken_exe=False):
        for version in versions:
            exe = b"This is not a Windows executable.\n" if broken_exe else self.sources[version].read_bytes()
            inventory = {"show23-pc-launcher.exe": exe, "ui/main.html": b"fixture page\n",
                         "bootstrap/show23-pc.exe": self.executable.read_bytes()}
            body = f"show23-pc version 1\nversion {version}\nchannel beta\nbootstrap 1\n"
            for name, data in sorted(inventory.items()):
                self.write(self.versions / version / name, data)
                body += f"file {hashlib.sha256(data).hexdigest()} {len(data)} {name}\n"
            self.write(self.versions / version / "version.txt", sealed(body))

    def seed(self, **fields):
        state = pointer(**fields)
        self.primary.write_bytes(state)
        self.backup.write_bytes(state)
        return state

    def retry_request(self):
        self.request.write_bytes(sealed(f"show23-pc request 1\nid {REQUEST_ID}\ntrial {NEW}\nreason retry\n"))

    def run(self, *, code=0, bad=None, kill=None, fail=None, dialog=None, action="confirm"):
        env = {key: value for key, value in os.environ.items() if not key.startswith("SHOW23_FIXTURE_")}
        env.update(SHOW23_FIXTURE_PORT="1", SHOW23_FIXTURE_ACTION=action)
        if bad:
            env.update(SHOW23_FIXTURE_BAD_VERSION=NEW, SHOW23_FIXTURE_BAD_MODE=bad)
        for key, value in (("KILL_AT", kill), ("FAIL_AT", fail), ("DIALOG", dialog)):
            if value:
                env[f"SHOW23_FIXTURE_{key}"] = value
        before = self.log.read_bytes() if self.log.exists() else b""
        result = subprocess.run([self.executable, "--update-fixture"], cwd=self.top, env=env, timeout=25)
        after = self.log.read_bytes() if self.log.exists() else b""
        assert after.startswith(before), self.log
        log = after[len(before):].decode("utf-8", errors="replace")
        error = self.data / "fixture-error.txt"
        assert result.returncode == code, (result.returncode, code, log, error.read_text() if error.exists() else None)
        assert all(path.read_bytes() == data for path, data in self.sentinels.items()), self.root
        return log

    @staticmethod
    def launches(log, *names):
        actual = [line.split("Z launch ", 1)[1] for line in log.splitlines() if "Z launch " in line]
        assert actual == list(names), (actual, names, log)
        return actual

    def trace(self):
        path = self.data / "fixture-launches.txt"
        if not path.exists():
            return []
        return [line.split() for line in path.read_text(encoding="ascii").splitlines()]

    def state(self, **expected):
        record = self.primary.read_bytes()
        lines = record.decode("ascii").splitlines()
        assert lines[0] == "show23-pc pointer 1" and record == sealed("\n".join(lines[:-1]) + "\n"), lines
        parsed = dict(line.split(" ", 1) for line in lines[1:-1] if not line.startswith("failed "))
        assert all(parsed[key.replace("_", "-")] == str(value) for key, value in expected.items()), parsed
        return record.decode("ascii")


def wait_fixture_child(f):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if (entries := f.trace()) and len(entries[-1]) == 3:
            break
        time.sleep(0.02)
    else:
        raise AssertionError(f"No fixture child recorded in {f.data / 'fixture-launches.txt'}")
    pid = int(entries[-1][-1])
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
    if not handle:
        assert ctypes.get_last_error() == 87, pid  # ERROR_INVALID_PARAMETER: PID already exited
        return
    try:
        assert kernel.WaitForSingleObject(handle, 10000) == 0, pid  # WAIT_OBJECT_0
    finally:
        kernel.CloseHandle(handle)


def hang_and_retry(ctx):
    f = Fixture(ctx, "E3-hang")
    f.stage(OLD, NEW)
    f.seed(current=OLD)
    log = f.run(bad="hang")
    first = f.launches(log, f"{NEW} (trial)", f"{NEW} (trial)", OLD)
    assert log.count(f"{NEW}: unconfirmed-timeout") == 2, log
    failed = f.state(current=OLD, trial="-", starts=0)
    assert f"failed {NEW} " in failed and " unconfirmed-timeout\n" in failed, failed
    assert f.launches(f.run(bad="hang"), OLD) == [OLD]
    assert f.primary.read_bytes() == failed.encode("ascii")
    f.launches(f.run(action="retry"), OLD)
    request = f.request.read_text(encoding="ascii")
    assert f"trial {NEW}\nreason retry\n" in request
    request_id = next(line[3:] for line in request.splitlines() if line.startswith("id "))
    log = f.run(bad="hang")
    retry = f.launches(log, f"{NEW} (trial)", f"{NEW} (trial)", OLD)
    assert log.count(f"{NEW}: unconfirmed-timeout") == 2 and not f.request.exists(), log
    after = f.state(current=OLD, trial="-", starts=0, last_request=request_id)
    assert " unconfirmed-timeout\n" in after, after
    observed = [entry[:2] for entry in f.trace()]
    assert observed == [[NEW, "trial"], [NEW, "trial"], [OLD, "current"], [OLD, "current"], [OLD, "current"],
                        [NEW, "trial"], [NEW, "trial"], [OLD, "current"]], observed
    return {"launches": first, "explicitRetryLaunches": retry, "requestId": request_id, "pointer": after}


def early_failure(ctx):
    evidence = {}
    for name, reason in (("crash", "exit:112"), ("missing-exe", "damaged:show23-pc-launcher.exe"),
                         ("create-process", "could-not-start:")):
        f = Fixture(ctx, f"E4-{name}")
        f.stage(OLD)
        f.stage(NEW, broken_exe=name == "create-process")
        if name == "missing-exe":
            (f.versions / NEW / "show23-pc-launcher.exe").unlink()
        f.seed(current=OLD, trial=NEW)
        log = f.run(bad="crash" if name == "crash" else None)
        expected = (OLD,) if name == "missing-exe" else (f"{NEW} (trial)", f"{NEW} (trial)", OLD)
        launches = f.launches(log, *expected)
        if name != "missing-exe":
            attempts = [line.rsplit(f"{NEW}: ", 1)[1] for line in log.splitlines() if f"{NEW}: {reason}" in line]
            assert len(attempts) == 2 and attempts[0] == attempts[1], log
            reason = attempts[-1]
            if name == "create-process":
                assert reason in ("could-not-start:193", "could-not-start:216"), reason
        state = f.state(current=OLD, trial="-", starts=0)
        assert f"failed {NEW} " in state and f" {reason}\n" in state, state
        evidence[name] = {"launches": launches, "pointer": state}
    return evidence


def corrupt_pointers(ctx):
    evidence = {}
    for name, damage in (("zero", b""), ("garbage", b"garbage\n"),
                         ("bad-end", pointer(current=NEW)[:-65] + b"0" * 64 + b"\n")):
        f = Fixture(ctx, f"E6-primary-{name}")
        f.stage(OLD)
        good = f.seed(current=OLD)
        f.primary.write_bytes(damage)
        f.launches(f.run(), OLD)
        assert f.primary.read_bytes() == f.backup.read_bytes() == good
        evidence[f"primary-{name}"] = f.state(current=OLD, trial="-", starts=0)

        f = Fixture(ctx, f"E6-both-{name}")
        f.stage(NEW)
        f.primary.write_bytes(damage)
        f.backup.write_bytes(damage)
        # Interrupt just before promotion. A rebuilt record must already have
        # counted this folder as a trial, even after the child signals readiness.
        log = f.run(code=3, fail="B8-confirm-primary")
        f.launches(log, f"{NEW} (trial)")
        assert "Z rebuilt" in log and "could not record confirmation" in log, log
        before = f.state(current="-", trial=NEW, starts=1)
        f.launches(f.run(), f"{NEW} (trial)")
        evidence[f"both-{name}"] = {"beforeConfirmation": before, "afterConfirmation": f.state(current=NEW, trial="-")}
    return evidence


@contextmanager
def write_protected_versions(f):
    system32 = os.path.join(os.environ["SystemRoot"], "System32")
    identity = subprocess.check_output([os.path.join(system32, "whoami.exe"), "/user", "/fo", "csv", "/nh"],
                                       text=True, errors="replace", timeout=10)
    _, sid = next(csv.reader(identity.strip().splitlines()))
    assert sid.startswith("S-1-"), identity

    def change(*args):
        return subprocess.run([os.path.join(system32, "icacls.exe"), f.versions, *args],
                              capture_output=True, text=True, errors="replace", timeout=10)

    # A directory's readonly attribute does not actually block Windows writes.
    # Deny adding files to this directory while leaving existing records readable.
    denied = change("/deny", f"*{sid}:(WD,AD)")
    if denied.returncode == 5:  # Managed sandboxes may deny access to change DACLs.
        # Windows denies MoveFileExW replacing a readonly destination. This
        # still exercises a real filesystem failure on the first record write.
        f.backup.chmod(stat.S_IREAD)
        try:
            yield "read-only pointer.bak"
        finally:
            f.backup.chmod(stat.S_IWRITE)
        return

    assert denied.returncode == 0, (denied.stdout, denied.stderr)
    try:
        probe = f.versions / "write-probe"
        try:
            probe.write_bytes(b"must be denied")
        except PermissionError:
            pass
        else:
            raise AssertionError(f"Windows permitted writes to {f.versions}")
        yield "versions directory ACL"
    finally:
        restored = change("/remove:d", f"*{sid}")
        assert restored.returncode == 0, (restored.stdout, restored.stderr)


def unavailable_folders(ctx):
    evidence = {}
    for name in ("missing", "incomplete"):
        f = Fixture(ctx, f"E7-{name}")
        f.stage(OLD)
        if name == "incomplete":
            f.stage(NEW)
            (f.versions / NEW / "ui/main.html").unlink()
        f.seed(current=NEW, previous=OLD)
        f.launches(f.run(), OLD)
        state = f.state(current=OLD, previous="-", trial="-", starts=0)
        reason = "version.txt" if name == "missing" else "ui/main.html"
        assert f"failed {NEW} " in state and f" damaged:{reason}\n" in state, state
        evidence[name] = state

    for name, current in (("read-only-with-current", OLD), ("read-only-no-current", "-")):
        f = Fixture(ctx, f"E7-{name}")
        f.stage(NEW)
        if current != "-":
            f.stage(OLD)
        before = f.seed(current=current, trial=NEW)
        f.log.write_bytes(b"")
        with write_protected_versions(f) as method:
            log = f.run(code=0 if current == OLD else 1)
            assert "couldn't record the update's start or state" in log, log
            if current == "-":
                assert "can't write the version record" in log, log
            launches = f.launches(log, *((OLD,) if current == OLD else ()))
        assert f.primary.read_bytes() == f.backup.read_bytes() == before
        evidence[name] = {"method": method, "launches": launches, "pointer": f.state(current=current, trial=NEW, starts=0)}

    f = Fixture(ctx, "E7-no-complete-version")
    f.stage(NEW)
    (f.versions / NEW / "ui/main.html").unlink()
    log = f.run(code=1)
    f.launches(log)
    state = f.state(current="-", trial="-", starts=0)
    assert "No complete launcher version could open" in log and " damaged:ui/main.html\n" in state, (log, state)
    evidence["no-complete-version"] = {"dialog": log, "pointer": state}
    return evidence


def pointer_cut(ctx, step):
    f = Fixture(ctx, f"E5-{step}")
    fields = {"current": OLD}
    if step == "B3":
        f.stage(OLD)
        fields = {"current": NEW, "previous": OLD}
    else:
        f.stage(OLD, NEW)
    if step == "B4":
        f.retry_request()
    if step == "B5-damaged":
        (f.versions / NEW / "ui/main.html").unlink()
    if step in {"B6-count", "B6-failed", "B8-confirm"}:
        fields["trial"] = NEW
    if step == "B6-failed":
        fields["starts"] = 2
    old = f.seed(**fields)
    stale = pointer(current=fields["current"], request=STALE_ID)
    f.backup.write_bytes(stale)
    cut = f.run(code=BOOTSTRAP_KILL, kill=f"{step}-backup")
    if step == "B8-confirm":
        f.launches(cut, f"{NEW} (trial)")
        old = pointer(current=OLD, trial=NEW, starts=1)
        wait_fixture_child(f)
    else:
        f.launches(cut)
    assert f.primary.read_bytes() == f.backup.read_bytes() == old and old != stale
    assert f.request.exists() == (step == "B4")
    recovered = f.run()
    if step in {"B3", "B5-damaged", "B6-failed"}:
        f.launches(recovered, OLD)
        after = f.state(current=OLD, previous="-", trial="-", starts=0)
        reason = {"B3": "damaged:version.txt", "B5-damaged": "damaged:ui/main.html",
                  "B6-failed": "interrupted-start"}[step]
        assert f" {reason}\n" in after, after
    else:
        f.launches(recovered, f"{NEW} (trial)")
        after = f.state(current=NEW, previous=OLD, trial="-", starts=0)
    assert not f.request.exists()
    return {"cutPointerAndBackup": old.decode("ascii"), "pointerAfterRestart": after, "restartLog": recovered}


def request_cut(ctx):
    f = Fixture(ctx, "E5-request-commit")
    f.stage(OLD, NEW)
    old = f.seed(current=OLD)
    f.retry_request()
    request = f.request.read_bytes()
    f.launches(f.run(code=BOOTSTRAP_KILL, kill="B4-request-commit"))
    before = f.state(current=OLD, trial=NEW, starts=0, last_request=REQUEST_ID)
    assert f.request.read_bytes() == request and f.backup.read_bytes() == old
    f.launches(f.run(bad="crash", kill="B4-backup"), f"{NEW} (trial)", f"{NEW} (trial)", OLD)
    after = f.state(current=OLD, trial="-", starts=0, last_request=REQUEST_ID)
    assert not f.request.exists() and " exit:112\n" in after, after
    # Replay that same committed id after exhaustion. Applying it again would
    # clear the recorded failure and give the broken folder two more starts.
    f.request.write_bytes(request)
    f.launches(f.run(bad="crash"), OLD)
    assert not f.request.exists() and f.primary.read_bytes() == after.encode("ascii")
    return {"afterCut": before, "afterRecoveryAndReplay": after}


def trial_cut(ctx):
    f = Fixture(ctx, "S8-trial-running")
    f.stage(OLD, NEW)
    f.seed(current=OLD, trial=NEW)
    log = f.run(code=BOOTSTRAP_KILL, bad="hang", kill="B7-trial-running")
    f.launches(log, f"{NEW} (trial)")
    wait_fixture_child(f)
    before = f.state(current=OLD, trial=NEW, starts=1)
    f.launches(f.run(bad="crash"), f"{NEW} (trial)", OLD)
    after = f.state(current=OLD, trial="-", starts=0)
    assert " exit:112\n" in after, after
    return {"countedBeforeKill": before, "afterRecovery": after}


def confirmation_failure(ctx):
    f = Fixture(ctx, "S8-confirm-write")
    f.stage(OLD, NEW)
    f.seed(current=OLD, trial=NEW)
    snapshots = []
    for count in (1, 2):
        log = f.run(code=3, fail="B8-confirm-primary")
        f.launches(log, f"{NEW} (trial)")
        assert "could not record confirmation" in log and f"Z confirmed {NEW}" not in log, log
        snapshots.append(f.state(current=OLD, trial=NEW, starts=count))
    f.launches(f.run(), OLD)
    after = f.state(current=OLD, trial="-", starts=0)
    assert f"failed {NEW} " in after and " interrupted-start\n" in after
    return {"countedStarts": snapshots, "fallback": after}


def counter_failure(ctx):
    evidence = {}
    for name, current in (("with-current", OLD), ("no-current", "-")):
        f = Fixture(ctx, f"S8-counter-write-{name}")
        f.stage(NEW)
        if current == OLD:
            f.stage(OLD)
        before = f.seed(current=current, trial=NEW)
        f.backup.write_bytes(pointer(current=current, request=STALE_ID))
        log = f.run(code=0 if current == OLD else 1, fail="B6-count-primary")
        launches = f.launches(log, *((OLD,) if current == OLD else ()))
        assert f.primary.read_bytes() == f.backup.read_bytes() == before
        assert "couldn't record the update's start or state" in log, log
        if current == OLD:
            notice = (f.versions / "notice.txt").read_text(encoding="ascii")
            assert "counter-write\n" in notice, notice
        else:
            assert "can't write the version record" in log, log
        evidence[name] = {"launches": launches, "pointer": f.state(current=current, trial=NEW, starts=0)}
    return evidence


def no_current_retry(ctx):
    f = Fixture(ctx, "S8-no-current-retry")
    f.stage(NEW)
    log = f.run(code=1, bad="crash", dialog="retry")
    launches = f.launches(log, *([f"{NEW} (trial)"] * 4))
    state = f.state(current="-", trial="-", starts=0)
    assert "last-request -\n" not in state and " exit:112\n" in state, state
    assert not f.request.exists() and log.count("No complete launcher version could open") == 2, log
    return {"attempts": launches, "dialogs": log, "pointer": state}


def current_failure(ctx):
    evidence = {}
    for name in ("launch", "crash", "hang", "previous"):
        f = Fixture(ctx, f"S8-D-current-{name}")
        f.stage(OLD)
        f.stage(NEW, broken_exe=name == "launch")
        before = f.seed(current=NEW, previous=OLD)
        log = f.run(code=0 if name == "previous" else 1,
                    bad={"crash": "crash", "hang": "hang", "previous": "crash"}.get(name),
                    dialog="previous" if name == "previous" else None)
        launches = f.launches(log, *((NEW, OLD) if name == "previous" else (NEW,)))
        reason = {"launch": "could-not-start:", "crash": "exit:112", "hang": "No window after",
                  "previous": "exit:112"}[name]
        assert f"show23-pc {NEW} did not open" in log and reason in log, log
        if name == "launch":
            reason = next(line.rsplit(" ", 1)[1] for line in log.splitlines() if "did not open could-not-start:" in line)
            assert reason in ("could-not-start:193", "could-not-start:216"), reason
        assert f.primary.read_bytes() == f.backup.read_bytes() == before
        if name == "hang":
            wait_fixture_child(f)
        evidence[name] = {"launches": launches, "reason": reason, "pointer": f.state(current=NEW, previous=OLD)}
    return evidence


def recover_bootstrap(ctx):
    evidence = {}
    for name in ("missing", "damaged"):
        f = Fixture(ctx, f"S8-bootstrap-{name}")
        f.stage(OLD)
        before = f.seed(current=OLD)
        if name == "missing":
            f.executable.unlink()
        else:
            f.executable.write_bytes(b"invalid bootstrap image\n")
        try:
            subprocess.run([f.executable, "--prepare"], cwd=f.top, timeout=5, check=True)
        except OSError as error:
            assert error.winerror in ((2,) if name == "missing" else (193, 216)), error
        else:
            raise AssertionError(f"Windows launched the {name} bootstrap")
        package_copy = f.versions / OLD / "bootstrap/show23-pc.exe"
        shutil.copyfile(package_copy, f.executable)
        f.launches(f.run(), OLD)
        assert f.primary.read_bytes() == f.backup.read_bytes() == before
        evidence[name] = {"restoredFrom": str(package_copy.relative_to(f.top)),
                          "sha256": hashlib.sha256(f.executable.read_bytes()).hexdigest()}
    return evidence


def run(ctx):
    ctx.record("E3", "Two hung starts stop, current runs, and one explicit retry tries the trial again.", lambda: hang_and_retry(ctx))
    ctx.record("E4", "Crash and Win32 create-process failure are counted; a missing executable is rejected before launch.",
               lambda: early_failure(ctx))
    ctx.record("E6", "Three forms of corruption restore a good backup, or rebuild the folder only as a trial.",
               lambda: corrupt_pointers(ctx))
    ctx.record("E7", "Previous recovers a missing current; no-current and a Windows write denial reach the right dialog.",
               lambda: unavailable_folders(ctx))
    for step in ("B3", "B4", "B5-arrival", "B5-damaged", "B6-count", "B6-failed", "B8-confirm"):
        ctx.record(f"E5-{step}-backup", "Kill after writing the backup, before primary; next start recovers.",
                   lambda step=step: pointer_cut(ctx, step))
    ctx.record("E5-B4-request-commit", "The committed request left behind by a kill is deleted without reapplying it.",
               lambda: request_cut(ctx))
    ctx.record("S8-trial-running", "A trial already running when the bootstrap is killed keeps its counted start.",
               lambda: trial_cut(ctx))
    ctx.record("S8-confirm-write", "A failed confirmation preserves both counted starts, then falls back to current.",
               lambda: confirmation_failure(ctx))
    ctx.record("S8-counter-write", "A failed counter write boots a valid current or shows D-write without a current.",
               lambda: counter_failure(ctx))
    ctx.record("S8-D-none-retry", "D-none offers one retry, with four total counted trial launches across both attempts.",
               lambda: no_current_retry(ctx))
    ctx.record("S8-D-current", "A failed current leaves the pointer unchanged, including when Previous is selected.",
               lambda: current_failure(ctx))
    ctx.record("S8-bootstrap-recovery", "The packaged recovery copy restores a missing or damaged bootstrap.",
               lambda: recover_bootstrap(ctx))
