"""Channel, verification, fallback and manual-selection regressions."""
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
from pathlib import Path
import shutil
import subprocess

from update_e2e_boot import Fixture, OLD, NEW
from update_e2e_review import put_in_slot


@contextmanager
def lock_file(path, share):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.CreateFileW(str(path), 0x80000000, share, None, 3, 0x80, None)
    assert handle not in (None, ctypes.c_void_p(-1).value)
    try:
        yield
    finally:
        kernel.CloseHandle(handle)


def beta_without_choice(ctx, lose_pointer):
    f = Fixture(ctx, "M6-beta-lost" if lose_pointer else "M6-beta-fresh")
    settings = f.data / "user/properties.json"
    f.sentinels[settings] = b'{"fixture":"sentinel"}\n'
    f.write(settings, f.sentinels[settings])
    f.stage(OLD)
    f.run()
    choice = f.versions / "channel.txt"
    initial = choice.read_text() if choice.exists() else None
    if not lose_pointer:
        assert initial is None, initial
        assert (f.data / "fixture-channel.txt").read_text() == "beta"
        return {"current": f.state(current=OLD), "channel": "beta", "saved": None}
    # A bare M5 mirror is not an explicit choice. Keep a newer intact Beta alongside the former current.
    f.write(choice, b"stable\n")
    f.stage(NEW)
    f.primary.unlink()
    f.backup.unlink()
    cutoff = f.run()
    f.launches(cutoff, f"{OLD} (trial)")
    assert choice.read_text() == "stable\n"
    return {"restored": f.state(current=OLD), "launch": cutoff}


def stable_without_choice(ctx, Copy, files, STABLE, BETA):
    c = Copy(ctx, "M6-stable-missing", STABLE, ctx.stable)
    c.start()
    (c.versions / "channel.txt").unlink(missing_ok=True)
    c.stage_target(BETA)
    for name, data in files(BETA, ctx.next_beta.read_bytes()).items():
        path = c.versions / BETA / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    before = c.state()
    c.start()
    assert c.state() == before and f"current {STABLE}\n" in before
    assert not (c.versions / "channel.txt").exists()
    assert (c.data / "fixture-channel.txt").read_text() == "stable"
    return {"preserved": before, "unscheduledBeta": BETA}


def channel_interruption(ctx):
    f = Fixture(ctx, "M6-channel-commit")
    f.stage(OLD)
    f.run()
    f.write(f.data / "fixture-mode", b"0.1.3")
    before = (f.data / "user/properties.json").read_bytes()
    f.run(action="switch", kill="channel-written", code=111)
    assert (f.versions / "channel.txt").read_bytes() == b"show23-pc channel 1\nstable\n"
    f.run()
    assert (f.data / "fixture-channel.txt").read_text() == "stable"
    assert (f.data / "user/properties.json").read_bytes() == before
    return {"effective": "stable", "propertiesUnchanged": True, "cutExit": 111}


def unreadable_arrival(ctx):
    f = Fixture(ctx, "M6-read-unknown")
    f.stage(OLD)
    f.run()
    f.stage(NEW)
    with lock_file(f.versions / NEW / "ui/main.html", 0):
        log = f.run()
        f.launches(log, OLD)
        assert f"failed {NEW} " not in f.state(current=OLD)
    recovered = f.run()
    f.launches(recovered, f"{NEW} (trial)")
    return {"whileLocked": log, "afterUnlock": recovered, "pointer": f.state(current=NEW)}


def changed_resource(ctx):
    f = Fixture(ctx, "M6-resource")
    f.stage(OLD)
    f.run()
    f.stage(NEW)
    f.run()
    f.state(current=NEW, previous=OLD)
    path = f.versions / NEW / "ui/main.html"
    original = path.read_bytes()
    f.write(path, b"*" + original[1:])
    log = f.run()
    f.launches(log, OLD)
    assert "damaged:ui/main.html" in f.state(current=OLD)
    return {"sameSizeResourceReplacement": True, "launch": log}


def budget_previous(ctx):
    f = Fixture(ctx, "M6-budget-previous")
    f.stage(OLD)
    f.run()
    f.stage(NEW)
    f.run()
    f.stage("0.1.4-beta.1", "0.1.5-beta.1", broken_exe=True)
    log = f.run(bad="crash", dialog="previous")
    names = f.launches(log, "0.1.5-beta.1 (trial)", "0.1.5-beta.1 (trial)", "0.1.4-beta.1 (trial)", NEW, OLD)
    return {"launches": names, "pointer": f.state(current=NEW, previous=OLD)}


def manual_locked(ctx):
    f = Fixture(ctx, "M6-manual-locked")
    f.stage(OLD)
    put_in_slot(f, OLD, "manual/play.cmd", b"@echo old\r\n")
    f.run()
    f.stage(NEW)
    put_in_slot(f, NEW, "manual/play.cmd", b"@echo damaged\r\n")
    f.run()
    pointer = f.primary.read_bytes()
    backup = f.backup.read_bytes()
    (f.versions / NEW / "show23-pc-launcher.exe").unlink()
    scripts = Path(__file__).resolve().parent / "manual-forwarders"
    for name in ("play.cmd", "select-version.ps1"):
        shutil.copyfile(scripts / name, f.top / "manual" / name)
    with lock_file(f.primary, 1):
        result = subprocess.run(r"cmd.exe /d /c manual\play.cmd ignored", cwd=f.top,
                                text=True, capture_output=True, timeout=25)
    assert result.returncode == 0 and result.stdout.strip() == "old", (result.stdout, result.stderr)
    assert f.primary.read_bytes() == pointer and f.backup.read_bytes() == backup
    assert not f.request.exists() and not (f.versions / "notice.txt").exists()
    return {"selected": result.stdout.strip(), "pointerUnchanged": True}


def run(ctx, Copy, release_files, STABLE, BETA):
    ctx.record("M6-beta-fresh", "A fresh Beta has effective Beta and writes no channel choice.", lambda: beta_without_choice(ctx, False))
    ctx.record("M6-beta-lost", "A lost Beta pointer recovers its last-confirmed version despite an old mirror.", lambda: beta_without_choice(ctx, True))
    ctx.record("M6-stable-missing", "Without a saved choice a running Stable excludes new Beta arrivals.",
               lambda: stable_without_choice(ctx, Copy, release_files, STABLE, BETA))
    ctx.record("M6-channel-commit", "Killing immediately after the channel write leaves a coherent selection.", lambda: channel_interruption(ctx))
    ctx.record("M6-read-unknown", "An unreadable arrival retries after the file can be read, with no durable failure.", lambda: unreadable_arrival(ctx))
    ctx.record("M6-resource", "A same-size changed UI resource fails confirmed startup verification.", lambda: changed_resource(ctx))
    ctx.record("M6-budget-previous", "A failed fourth launch still offers a verified previous version.", lambda: budget_previous(ctx))
    ctx.record("M6-manual-locked", "A manual command selects verified previous without writing the locked pointer.", lambda: manual_locked(ctx))
