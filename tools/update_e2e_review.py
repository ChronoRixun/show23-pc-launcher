"""Disposable regressions for versioned update startup recovery."""
import ctypes
from ctypes import wintypes
import hashlib

from update_e2e_boot import Fixture, OLD, NEW, sealed, wait_fixture_child


def put_in_slot(fixture, version, filename, data):
    slot = fixture.versions / version
    fixture.write(slot / filename, data)
    files = {p.relative_to(slot).as_posix(): p.read_bytes() for p in slot.rglob("*")
             if p.is_file() and p.name != "version.txt"}
    body = f"show23-pc version 1\nversion {version}\nchannel beta\nbootstrap 1\n"
    body += "".join(f"file {hashlib.sha256(data).hexdigest()} {len(data)} {name}\n"
                    for name, data in sorted(files.items()))
    fixture.write(slot / "version.txt", sealed(body))


def orphan(ctx):
    f = Fixture(ctx, "R1-orphan")
    f.stage(OLD)
    f.run()
    f.stage(NEW)
    try:
        log = f.run(bad="hang", kill="B7-trial-running", code=111)
        f.launches(log, f"{NEW} (trial)")
        f.state(current=OLD, trial=NEW, starts=1)
        assert f.trace()[-1][:2] == [NEW, "trial"]  # The killed trial had taken the app mutex.
        log = f.run(bad="hang")
        f.launches(log, f"{NEW} (trial)", OLD)
        return {"reopenedImmediately": True, "log": log, "pointer": f.state(current=OLD, trial="-", starts=0)}
    finally:
        # Baseline leaves an orphan. Clean up before the next case.
        if f.trace():
            wait_fixture_child(f)


def confirmed_fallback(ctx):
    f = Fixture(ctx, "R2-confirmed-fallback")
    f.stage(OLD)
    f.run()
    f.stage(NEW)
    f.run()
    state = f.state(current=NEW, previous=OLD, trial="-")
    (f.versions / NEW / "show23-pc-launcher.exe").unlink()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    lock = kernel.CreateFileW(str(f.primary), 0x80000000, 1, None, 3, 0x80, None)
    assert lock not in (None, ctypes.c_void_p(-1).value)
    try:
        log = f.run()
        f.launches(log, OLD)
        assert f.primary.read_bytes() == state.encode()
        assert b"fallback-write" in (f.versions / "notice.txt").read_bytes()
    finally:
        kernel.CloseHandle(lock)
    f.run()
    return {"lockDeniedPointerReplacement": True, "launch": log, "repairedAfterUnlock": f.state(current=OLD)}


def repacked_current(ctx):
    f = Fixture(ctx, "R3-repack")
    f.stage(OLD)
    f.run()
    f.state(current=OLD, trial="-")
    put_in_slot(f, OLD, "ui/main.html", b"repacked UI with a new sealed inventory\n")
    log = f.run()
    f.launches(log, f"{OLD} (trial)")
    return {"launch": log, "pointer": f.state(current=OLD, trial="-", starts=0)}


def damaged_binary(ctx):
    f = Fixture(ctx, "R3-same-size")
    f.stage(OLD)
    f.run()
    f.stage(NEW)
    name = "cef/release/libcef.dll"
    put_in_slot(f, NEW, name, b"same size DLL a\n")
    f.run()
    f.state(current=NEW, previous=OLD)
    f.write(f.versions / NEW / name, b"same size DLL b\n")
    log = f.run()
    f.launches(log, OLD)
    pointer = f.state(current=OLD, trial="-")
    assert f"damaged:{name}" in pointer
    return {"launch": log, "pointer": pointer, "sizeUnchanged": True}


def attempt_limit(ctx):
    f = Fixture(ctx, "R6-budget")
    f.stage(OLD)
    f.run()
    earlier = NEW
    later = "0.1.4-beta.1"
    f.stage(earlier, later, broken_exe=True)
    log = f.run()
    names = f.launches(log, f"{later} (trial)", f"{later} (trial)", f"{earlier} (trial)", OLD)
    return {"starts": names, "pointer": f.state(current=OLD, trial=earlier, starts=1)}


def run(ctx):
    ctx.record("R1", "Killing the bootstrap frees the hung trial and an immediate reopen retries then falls back.",
               lambda: orphan(ctx))
    ctx.record("R2", "A locked pointer permits the verified previous folder to run with a notice, then retries the write.",
               lambda: confirmed_fallback(ctx))
    ctx.record("R3-repack", "Different bytes in a previously confirmed folder run as a new trial.",
               lambda: repacked_current(ctx))
    ctx.record("R3-dll", "A same-size damaged CEF DLL is detected before launching confirmed current.",
               lambda: damaged_binary(ctx))
    ctx.record("R6", "Multiple failing arrivals reserve the fourth launch for confirmed current.",
               lambda: attempt_limit(ctx))
