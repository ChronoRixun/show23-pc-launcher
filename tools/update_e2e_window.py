"""Read and close only windows owned by a disposable launcher."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import time
import uuid

user = ctypes.WinDLL("user32", use_last_error=True)
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
CALLBACK = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user.EnumWindows.argtypes = (CALLBACK, wintypes.LPARAM)
user.IsWindowVisible.argtypes = (wintypes.HWND,)
user.IsIconic.argtypes = (wintypes.HWND,)
user.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
user.GetForegroundWindow.restype = wintypes.HWND
user.GetPropW.argtypes = (wintypes.HWND, wintypes.LPCWSTR)
user.GetPropW.restype = wintypes.HANDLE
user.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.QueryFullProcessImageNameW.argtypes = (
    wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                              wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.ReadFile.argtypes = (wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                            ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)

class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
        ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
        ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
        ("image", wintypes.WCHAR * 260),
    ]


kernel.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
kernel.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
kernel.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)


def path_from_handle(handle):
    length = wintypes.DWORD(32768)
    value = ctypes.create_unicode_buffer(length.value)
    if kernel.QueryFullProcessImageNameW(handle, 0, value, ctypes.byref(length)):
        return Path(value.value)


def process_path(pid):
    handle = kernel.OpenProcess(0x1000, False, pid)
    try:
        return path_from_handle(handle) if handle else None
    finally:
        if handle:
            kernel.CloseHandle(handle)


def in_portable_copy(path, top):
    if path is None:
        return False
    return (path == top / "show23-pc.exe" or
            (path.name.casefold() == "show23-pc-launcher.exe" and
             path.is_relative_to(top / "show23-pc/versions")))


def owned_processes(top):
    top = top.resolve()
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot in (None, ctypes.c_void_p(-1).value):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = ProcessEntry()
        entry.size = ctypes.sizeof(entry)
        processes = {}
        found = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while found:
            if entry.image.casefold() in {"show23-pc.exe", "show23-pc-launcher.exe"}:
                path = process_path(entry.pid)
                if in_portable_copy(path, top):
                    processes[entry.pid] = path
            found = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        return processes
    finally:
        kernel.CloseHandle(snapshot)


def wait_for_launcher_exit(top):
    remaining = owned_processes(top)
    if not remaining:
        return {"afterMainExit": [], "forced": [], "remaining": 0}
    # The original portable launcher's CEF children can use the very same EXE
    # as the main process. Let watchdog shutdown complete before overwriting it.
    forced = []
    try:
        wait_for(lambda: not owned_processes(top), 10, "all owned launcher processes to exit")
    except AssertionError:
        for pid, original in owned_processes(top).items():
            handle = kernel.OpenProcess(0x101001, False, pid)
            if not handle:
                continue
            try:
                # Verify identity on the same handle used to terminate, in case the PID was reused.
                path = path_from_handle(handle)
                if path == original and in_portable_copy(path, top):
                    if kernel.TerminateProcess(handle, 1):
                        forced.append(pid)
                        kernel.WaitForSingleObject(handle, 5000)
            finally:
                kernel.CloseHandle(handle)
        wait_for(lambda: not owned_processes(top), 5, "owned launcher cleanup")
    return {"afterMainExit": [{"pid": pid, "image": path.name} for pid, path in remaining.items()],
            "forced": forced, "remaining": 0}


def owned_window(executable, require_root=False):
    result = None

    @CALLBACK
    def check(hwnd, _):
        nonlocal result
        if not user.IsWindowVisible(hwnd):
            return True
        if require_root and user.GetPropW(hwnd, "show23_root_window") != hwnd:
            return True
        pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        path = process_path(pid.value)
        if path == executable:
            result = hwnd, pid.value
            return False
        return True

    user.EnumWindows(check, 0)
    return result


def wait_for(callback, seconds=30, description="state"):
    ends = time.monotonic() + seconds
    while time.monotonic() < ends:
        value = callback()
        if value:
            return value
        time.sleep(.025)
    raise AssertionError(f"Timed out waiting for {description}")


def read_pointer(path):
    # An observer must not block the bootstrap's durable rename.
    handle = kernel.CreateFileW(str(path), 0x80000000, 7, None, 3, 0x00200000, None)
    if handle in (None, ctypes.c_void_p(-1).value):
        return ""
    try:
        data = ctypes.create_string_buffer(8193)
        size = wintypes.DWORD()
        if not kernel.ReadFile(handle, data, len(data), ctypes.byref(size), None):
            return ""
        return data.raw[:size.value].decode("ascii")
    finally:
        kernel.CloseHandle(handle)


def start_offline(top):
    top = top.resolve()
    data = top / "show23-pc"
    if not (data / "portable.marker").is_file():
        raise ValueError("A visible test requires a disposable portable extraction.")
    props = data / "user/properties.json"
    settings = json.loads(props.read_bytes()) if props.is_file() else {}
    if settings.get("launcher-auto-shortcuts") != "false":
        raise ValueError("Disable auto-shortcuts in the disposable fixture before visible startup.")
    with (top / "cef-startup.stderr").open("wb") as log:
        return subprocess.Popen([top / "show23-pc.exe", "-offline", "--enable-logging=stderr"], cwd=top,
                                stdin=subprocess.DEVNULL, stdout=log, stderr=log)


def taskbar_target(hwnd):
    class Key(ctypes.Structure):
        _fields_ = [("format", ctypes.c_ubyte * 16), ("id", wintypes.DWORD)]

    def guid(value):
        return (ctypes.c_ubyte * 16).from_buffer_copy(uuid.UUID(value).bytes_le)

    ole = ctypes.WinDLL("ole32")
    shell = ctypes.WinDLL("shell32")
    ole.CoInitializeEx.argtypes = (ctypes.c_void_p, wintypes.DWORD)
    ole.PropVariantClear.argtypes = (ctypes.c_void_p,)
    shell.SHGetPropertyStoreForWindow.argtypes = (
        wintypes.HWND, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
    if ole.CoInitializeEx(None, 2) < 0:
        raise AssertionError("Could not start COM for taskbar observation")
    try:
        store = ctypes.c_void_p()
        iid = guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99")
        result = shell.SHGetPropertyStoreForWindow(hwnd, iid, ctypes.byref(store))
        if result < 0 or not store:
            raise AssertionError(f"Could not read taskbar property store: 0x{result & 0xffffffff:08x}")
        vtable = ctypes.cast(store, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
        release = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable[2])
        get = ctypes.WINFUNCTYPE(wintypes.LONG, ctypes.c_void_p, ctypes.POINTER(Key), ctypes.c_void_p)(vtable[5])
        try:
            key = Key(guid("9f4c2855-9f79-4b39-a8d0-e1d42de1d5f3"), 2)
            value = (ctypes.c_uint64 * 3)()
            result = get(store, ctypes.byref(key), value)
            try:
                if result < 0:
                    raise AssertionError(f"Could not read relaunch command: 0x{result & 0xffffffff:08x}")
                kind = value[0] & 0xffff
                if kind == 0:
                    return None
                if kind != 31 or not value[1]:
                    raise AssertionError("The taskbar relaunch command is not a string")
                return ctypes.wstring_at(value[1])
            finally:
                ole.PropVariantClear(value)
        finally:
            release(store)
    finally:
        ole.CoUninitialize()


def wait_for_taskbar_target(hwnd, read=taskbar_target, seconds=8):
    missing = 0

    def poll():
        nonlocal missing
        target = read(hwnd)
        if not target:
            missing += 1
        return target

    target = wait_for(poll, seconds, "taskbar relaunch command on the launcher root window")
    return target, missing


def verify_confirmation(before, after, observations, version, already_current):
    line = f"current {version}\n"
    assert line in after and "trial -\nstarts 0\n" in after
    assert any(row["visible"] for row in observations)
    if already_current:
        assert line in before and before == after, "A restored confirmed pointer changed"
    else:
        assert line not in before, "Expected a fresh trial"
        assert not any(row["current"] == version and not row["visible"] for row in observations), observations


def confirm(top, version=None, capture=None, delegate=None, second=False, already_current=False):
    top = top.resolve()
    versions = top / "show23-pc/versions"
    pointer = versions / "pointer.txt"
    exe = versions / version / "show23-pc-launcher.exe" if version else top / "show23-pc.exe"
    before = read_pointer(pointer)
    if not exe.is_file():
        raise ValueError(f"Missing fixture executable: {exe.name}")
    assert not owned_processes(top), "This disposable copy already has a running launcher"
    app = start_offline(top)
    window = None
    evidence = {}
    try:
        while_pending = []
        start = time.monotonic()

        def check():
            known = owned_window(exe, require_root=version is not None)
            state = read_pointer(pointer)
            elapsed = round((time.monotonic() - start) * 1000)
            item = {"ms": elapsed, "visible": bool(known),
                    "current": next((line[8:] for line in state.splitlines() if line.startswith("current ")), None),
                    "trial": next((line[6:] for line in state.splitlines() if line.startswith("trial ")), None)}
            if not while_pending or any(item[k] != while_pending[-1][k] for k in ("visible", "current", "trial")):
                while_pending.append(item)
            if app.poll() is not None:
                raise AssertionError(f"Launcher exited with {app.returncode} before a visible confirmed startup")
            if known and (not version or f"current {version}\n" in state):
                return known

        window = wait_for(check, 38, "owned launcher window and confirmation")
        hwnd, pid = window
        evidence = {"pid": pid, "beforePointer": before, "afterPointer": read_pointer(pointer),
                    "observations": while_pending}
        if version:
            verify_confirmation(before, evidence["afterPointer"], while_pending, version, already_current)
            evidence["startupKind"] = "already-current" if already_current else "trial-confirmed"
            target, missing = wait_for_taskbar_target(hwnd)
            assert Path(target.strip('"')) == top / "show23-pc.exe", target
            evidence["taskbarRelaunch"] = target
            evidence["taskbarReadsBeforePublication"] = missing
        if capture:
            picture = subprocess.run(["python", str(delegate), "capture", str(capture),
                                      "--process", exe.name, "--pid", str(pid)],
                                     capture_output=True, text=True, timeout=25)
            evidence["capture"] = {"status": picture.returncode, "output": picture.stdout.strip()}
            if picture.returncode or not capture.is_file() or "NOT CAPTURED" in picture.stdout or "blank" in picture.stdout.lower():
                raise AssertionError(f"Capture failed: {picture.stdout} {picture.stderr}")
        if second:
            if not user.PostMessageW(hwnd, 0x0112, 0xF020, 0):
                raise AssertionError("Could not minimize the owned test window")
            wait_for(lambda: user.IsIconic(hwnd), 5, "owned launcher minimize")
            # Directly starting the slot must hand off to the existing bootstrap.
            sub = subprocess.Popen([exe, "-offline"], cwd=top, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                assert sub.wait(timeout=8) == 0
                wait_for(lambda: not user.IsIconic(hwnd), 5, "owned launcher restoration")
                foreground = wait_for(lambda: user.GetForegroundWindow() == hwnd, 5, "owned launcher foreground")
                assert foreground
                assert owned_window(exe, require_root=version is not None) == window
                wait_for(lambda: not any(process != app.pid and path == top / "show23-pc.exe"
                                         for process, path in owned_processes(top).items()),
                         8, "handoff bootstrap exit while the original window stays open")
            finally:
                if sub.poll() is None:
                    sub.kill()
                    sub.wait(timeout=5)
            evidence["secondStart"] = {"directHandoff": True, "restored": True, "foreground": True}
        return evidence
    finally:
        # Never send a message to a window from a different extracted folder.
        window = window or owned_window(exe)
        if window:
            user.PostMessageW(window[0], 0x0010, 0, 0)
        try:
            app.wait(timeout=8 if window else 1)
        except subprocess.TimeoutExpired:
            app.kill()
            app.wait(timeout=5)
        evidence["shutdown"] = wait_for_launcher_exit(top)
