"""Loopback updater cases for retained copies of the offered release."""

import hashlib
import json


def _with_different_ui(files):
    changed = dict(files)
    name = "ui/main.html"
    original = files[name]
    replacement = b"Retained copy of the same release\n"

    def row(data):
        return f"file {hashlib.sha256(data).hexdigest()} {len(data)} {name}\n".encode()

    body, _ = files["version.txt"].rsplit(b"\nend ", 1)
    body += b"\n"
    assert row(original) in body
    body = body.replace(row(original), row(replacement))
    changed[name] = replacement
    changed["version.txt"] = body + f"end {hashlib.sha256(body).hexdigest()}\n".encode()
    return changed


def same_tag(ctx, case, same, Copy, release_files, feed, expect_current, transient, write_files, new, old,
             *, competing=False):
    copy = Copy(ctx, case)
    copy.start()
    expect_current(copy, old)
    copy.stage_target(new)
    files = release_files(new, ctx.new.read_bytes())
    different = _with_different_ui(files)
    retained = {new: files if same else different}
    if competing:
        retained[new + "~4"] = different
    selected = new + "~5" if competing else new if same else new + "~2"
    old_receipts = {slot: f"previous receipt for {slot}\n".encode() for slot in retained}

    def stage_retained_copies():
        # The old launcher is already running when it requests the feed. Its bootstrap has not seen these slots.
        for slot, contents in retained.items():
            write_files(copy.versions / slot, contents)
            (copy.versions / f"{slot}.launcher.json").write_bytes(old_receipts[slot])

    with feed(new, files, before_listing=stage_retained_copies) as (port, requests, manifest):
        copy.start(port=port, action="update")
    pointer = expect_current(copy, selected, old)
    assert len(requests) == 4 and requests[-1].endswith(".zip"), requests
    assert json.loads((copy.versions / f"{selected}.launcher.json").read_bytes()) == manifest
    assert {path.name for path in copy.versions.iterdir() if path.is_dir() and path.name != "confirmed"} == {old, selected, *retained}
    for slot, contents in {old: copy.files, **retained, selected: files}.items():
        for name, data in contents.items():
            assert (copy.versions / slot / name).read_bytes() == data, (slot, name)
    for slot, receipt in old_receipts.items():
        if slot != selected:
            assert (copy.versions / f"{slot}.launcher.json").read_bytes() == receipt, slot
    assert not transient(copy) and not (copy.versions / "request.txt").exists()
    return {"pointer": pointer, "requests": requests, "selected": selected, "retained": sorted(retained),
            **copy.check()}
