"""Exercise the archived shell forwarders without starting any game's processes."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
from uuid import uuid4
from zipfile import ZipFile


def seal(body):
    return body + f"end {hashlib.sha256(body).hexdigest()}\n".encode()


def check(package, work, shell):
    root = work / ("manual-" + uuid4().hex)
    root.mkdir()
    try:
        with ZipFile(package) as archive:
            archive.extractall(root)
        top = root / package.stem
        bootstrap = top / "show23-pc.exe"
        versions = top / "show23-pc/versions"
        pointer = versions / "pointer.txt"
        (old,) = list(versions.iterdir())
        # chmod proves unreadability on non-root Linux. Replace the PE as well to
        # test the same forwarding behavior in a Windows POSIX shell.
        bootstrap.write_bytes(b"not a Windows executable\n")
        bootstrap.chmod(0)
        if os.name != "nt" and (os.access(bootstrap, os.R_OK) or os.access(bootstrap, os.X_OK)):
            raise AssertionError("Run this test as an unprivileged Linux user.")
        env = os.environ.copy()
        if os.name == "nt":
            shim = top / "_test-path"
            shim.mkdir()
            (shim / "python3").write_bytes(f'#!/bin/sh\nexec "{Path(sys.executable).as_posix()}" "$@"\n'.encode())
            env["PATH"] = str(shim) + os.pathsep + env["PATH"]

        def run(name, args=()):
            return subprocess.run([shell, "manual/" + name, *args], cwd=top, env=env,
                                  capture_output=True, text=True, timeout=20)

        for name in ("setup.sh", "play.sh", "import-cards.sh"):
            result = run(name)
            assert result.returncode == 2 and f"Usage: sh {name}" in result.stderr, result
        assert not pointer.exists()
        print("Fresh ZIP: setup.sh, play.sh, and import-cards.sh reached their packaged usage with no pointer.")

        new = versions / "0.1.4"
        (new / "manual").mkdir(parents=True)
        (new / "show23-pc-launcher.exe").write_bytes(b"new")
        for folder, label in ((old, "current"), (new, "new")):
            (folder / "manual/play.sh").write_bytes(f'printf "%s\\n" "{label}" "$@"\n'.encode())
            base = folder.name.split("~")[0]
            channel = "beta" if "-" in base else "stable"
            body = f"show23-pc version 1\nversion {base}\nchannel {channel}\nbootstrap 1\n"
            for file in sorted(folder.rglob("*")):
                if file.is_file() and file.name != "version.txt":
                    content = file.read_bytes()
                    body += f"file {hashlib.sha256(content).hexdigest()} {len(content)} {file.relative_to(folder).as_posix()}\n"
            (folder / "version.txt").write_bytes(seal(body.encode()))
        args = ["install with spaces", "emulator with spaces", "--name", "Player One"]
        actual = run("play.sh", args)
        assert actual.returncode == 0 and actual.stdout.splitlines() == ["new", *args], actual

        body = seal(
            f"show23-pc pointer 1\ncurrent {old.name}\nprevious -\ntrial 0.1.4\nstarts 1\nlast-request -\n".encode()
        )
        pointer.write_bytes(body)
        actual = run("play.sh", args)
        assert actual.returncode == 0 and actual.stdout.splitlines() == ["current", *args], actual
        assert pointer.read_bytes() == body
        script = old / "manual/play.sh"
        good = script.read_bytes()
        script.write_bytes(b"!" + good[1:])
        actual = run("play.sh", args)
        assert actual.returncode == 1 and "No verified launcher version" in actual.stderr, actual
        script.write_bytes(good)

        # A torn pointer and an invalid higher folder must leave the valid old folder usable.
        pointer.write_text("torn")
        (new / "version.txt").write_text("torn")
        actual = run("play.sh", args)
        assert actual.returncode == 0 and actual.stdout.splitlines() == ["current", *args], actual
        assert pointer.read_text() == "torn"
        bootstrap.chmod(0o600)
        print("Highest eligible folder, current over trial, unchanged argument boundaries, and torn-record fallback passed.")
    finally:
        for item in root.glob("*/show23-pc.exe"):
            item.chmod(0o600)
        shutil.rmtree(root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("work", type=Path)
    parser.add_argument("--shell", default="sh")
    options = parser.parse_args()
    check(options.package.resolve(), options.work.resolve(), options.shell)
