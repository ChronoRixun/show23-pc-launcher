"""Check Windows manual forwarders and version selection without starting the game."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
from uuid import uuid4
from zipfile import ZipFile


def seal(body):
    return body + f"end {hashlib.sha256(body).hexdigest()}\n".encode()


def fixture_version(folder, label):
    (folder / "manual").mkdir(parents=True, exist_ok=True)
    (folder / "manual/play.cmd").write_bytes(
        f'@echo off\r\necho {label}\r\necho %~1\r\necho %~2\r\necho %~3\r\n'.encode())
    body = f"show23-pc version 1\nversion {folder.name}\nchannel stable\nbootstrap 1\n"
    for file in sorted(folder.rglob("*")):
        if file.is_file() and file.name != "version.txt":
            data = file.read_bytes()
            body += f"file {hashlib.sha256(data).hexdigest()} {len(data)} {file.relative_to(folder).as_posix()}\n"
    data = seal(body.encode())
    (folder / "version.txt").write_bytes(data)
    return data.splitlines()[-1].decode("ascii").removeprefix("end ")


def check(package, work):
    root = work / ("manual-" + uuid4().hex)
    root.mkdir()
    try:
        with ZipFile(package) as archive:
            archive.extractall(root)
        top = root / package.stem
        versions = top / "show23-pc/versions"
        pointer = versions / "pointer.txt"
        (old,) = [folder for folder in versions.iterdir() if folder.is_dir()]
        assert "-" not in old.name, "Use the stable native portable ZIP."
        new = versions / "99.0.0"
        (new / "manual").mkdir(parents=True)
        (new / "show23-pc-launcher.exe").write_bytes(b"fixture; never executed")
        fingerprint = fixture_version(old, "current")
        fixture_version(new, "trial")

        def run():
            return subprocess.run(r'cmd.exe /d /c manual\play.cmd "install with spaces" --name "Player One"',
                                  cwd=top, capture_output=True, text=True, timeout=20)

        expected = ["install with spaces", "--name", "Player One"]
        assert not pointer.exists()
        result = run()
        assert result.returncode == 0 and result.stdout.splitlines() == ["trial", *expected], result

        confirmed = versions / "confirmed"
        confirmed.mkdir()
        (confirmed / (old.name + ".txt")).write_bytes(seal(
            f"show23-pc confirmed 1\nfolder {old.name}\ninventory {fingerprint}\n".encode()))
        body = seal(f"show23-pc pointer 1\ncurrent {old.name}\nprevious -\ntrial {new.name}\nstarts 1\nlast-request -\n".encode())
        pointer.write_bytes(body)
        result = run()
        assert result.returncode == 0 and result.stdout.splitlines() == ["current", *expected], result
        assert pointer.read_bytes() == body

        (new / "version.txt").write_text("torn", encoding="ascii")
        pointer.write_text("torn", encoding="ascii")
        result = run()
        assert result.returncode == 0 and result.stdout.splitlines() == ["current", *expected], result
        assert pointer.read_text(encoding="ascii") == "torn"
        print("3 manual forwarding checks passed.")
    finally:
        shutil.rmtree(root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("work", type=Path)
    args = parser.parse_args()
    check(args.package.resolve(), args.work.resolve())
