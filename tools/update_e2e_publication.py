"""Exercise updater publication and explicit requests with the real bootstrap."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import time

from update_e2e_boot import pointer
from update_e2e_reuse import _with_different_ui


@contextmanager
def deny_replacement(path):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x80, None)
    if handle in (None, ctypes.c_void_p(-1).value):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        yield
    finally:
        kernel.CloseHandle(handle)


def same_tag_pending(ctx, case, Copy, release_files, feed, expect_current, transient, write_files,
                     old, new, *, interrupted=False):
    copy = Copy(ctx, case)
    copy.start()
    expect_current(copy, old)
    copy.stage_target(new)
    files = release_files(new, ctx.new.read_bytes())
    stale_files = _with_different_ui(files)
    selected = new + "~2"

    def seed_pending():
        # The launcher is already open below the offered tag, and the bootstrap hasn't seen
        # the retained trial. Give that trial different verified bytes and an attempted start.
        write_files(copy.versions / new, stale_files)
        (copy.versions / (new + ".launcher.json")).write_bytes(b"retained manifest\n")
        copy.pointer.write_bytes(pointer(current=old, trial=new, starts=1))

    with feed(new, files, before_listing=seed_pending) as (port, requests, _):
        cut = "U5-after-rename" if interrupted else None
        copy.start(port=port, action="update", cut=cut, expect=111 if interrupted else 0)
    if interrupted:
        assert f"trial {new}\nstarts 1\n" in copy.state()
        assert (copy.versions / selected).is_dir()
        copy.start()

    trace = (copy.data / "fixture-launches.txt").read_text(encoding="ascii")
    assert [line.split()[:2] for line in trace.splitlines()] == [
        [old, "trial"], [old, "current"], [selected, "trial"]], trace
    state = expect_current(copy, selected, old)
    assert len(requests) == 4 and requests[-1].endswith(".zip"), requests
    assert not transient(copy)
    for folder, contents in ((new, stale_files), (selected, files)):
        for name, data in contents.items():
            assert (copy.versions / folder / name).read_bytes() == data, (folder, name)
    assert (copy.versions / (new + ".launcher.json")).read_bytes() == b"retained manifest\n"
    return {"selected": selected, "retained": new, "interrupted": interrupted,
            "pointer": state, **copy.check()}


def request_waits(ctx, Copy, release_files, feed, expect_current, stable, beta):
    copy = Copy(ctx, "M6-request-wait", stable, ctx.stable)
    copy.start()
    copy.stage_target(beta)
    with feed(beta, release_files(beta, ctx.next_beta.read_bytes())) as (port, _, _):
        copy.start(port=port, action="update", SHOW23_FIXTURE_CHANNEL="beta")
    expect_current(copy, beta, stable)

    # Leave the last confirmed Beta healthy, but make the record name a missing current
    # slot. A lock on pointer.txt forces the bootstrap to open that verified previous.
    missing = beta + "~2"
    stale = pointer(current=missing, previous=beta)
    copy.pointer.write_bytes(stale)
    (copy.versions / "pointer.bak").write_bytes(stale)
    copy.stage_target(stable)
    log = copy.data / "data/logs/show23-pc.log"
    request = copy.versions / "request.txt"
    with feed(stable, copy.files) as (port, requests, _), ThreadPoolExecutor(max_workers=1) as worker:
        with deny_replacement(copy.pointer):
            switched = worker.submit(copy.start, port=port, action="switch")
            deadline = time.monotonic() + 8
            while b"launcher update: waiting to record the update request" not in log.read_bytes():
                if switched.done():
                    switched.result()
                    raise AssertionError("The switch finished while the selection record was locked.")
                if time.monotonic() >= deadline:
                    raise AssertionError("The launcher did not wait on the locked selection record.")
                time.sleep(0.05)
            assert copy.pointer.read_bytes() == stale
            assert not request.exists()
        switched.result()

    state = expect_current(copy, stable, beta)
    assert len(requests) == 1 and requests[0].endswith("/latest"), requests
    assert not request.exists()
    used_id = next(line.split()[1] for line in state.splitlines() if line.startswith("last-request "))
    assert len(used_id) == 16
    return {"pointer": state, "request": used_id, "requests": requests,
            **copy.check()}


def run(ctx, Copy, release_files, feed, expect_current, transient, write_files, old, new, stable, beta):
    for case, interrupted in (("M6-publication", False), ("M6-publication-cut", True)):
        ctx.record(case, "The newly published same-tag bytes start ahead of the stale pending trial.",
                   lambda case=case, interrupted=interrupted: same_tag_pending(
                       ctx, case, Copy, release_files, feed, expect_current, transient, write_files, old, new,
                       interrupted=interrupted))
    ctx.record("M6-request-wait", "A Stable switch waits for the locked pointer and succeeds after unlock.",
               lambda: request_waits(ctx, Copy, release_files, feed, expect_current, stable, beta))
