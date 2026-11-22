"""Pointer recovery from retained confirmation records."""
from update_e2e_boot import Fixture, NEW, OLD, sealed
from update_e2e_review import put_in_slot


STABLE = "0.1.3"


def recover(ctx, *, repacked):
    f = Fixture(ctx, "M7-history-repacked" if repacked else "M7-history-older-beta")
    f.stage(OLD)
    first = f.run()
    assert f"Z confirmed {OLD}" in first, first

    f.sources[STABLE] = ctx.stable
    f.stage(STABLE)
    # The bootstrap fixture's stage helper emits a Beta inventory.
    manifest = f.versions / STABLE / "version.txt"
    body = manifest.read_text(encoding="ascii").rsplit("end ", 1)[0].replace("channel beta\n", "channel stable\n")
    manifest.write_bytes(sealed(body))
    latest = f.run()
    assert f"Z confirmed {STABLE}" in latest, latest
    f.state(current=STABLE, previous=OLD, trial="-")

    f.stage(NEW)
    channel = f.versions / "channel.txt"
    f.write(channel, b"show23-pc channel 1\nstable\n")
    stable_page = f.versions / STABLE / "ui/main.html"
    contents = stable_page.read_bytes()
    stable_page.write_bytes(b"*" + contents[1:])
    if repacked:
        put_in_slot(f, OLD, "ui/main.html", b"Changed Beta with a newly sealed inventory\n")
    f.primary.unlink()
    f.backup.unlink()

    launch = f.run(code=1 if repacked else 0)
    expected_launches = () if repacked else (f"{OLD} (trial)",)
    f.launches(launch, *expected_launches)
    state = f.state(current="-" if repacked else OLD, previous="-", trial="-", starts=0)
    assert f"failed {STABLE} " in state and "damaged:ui/main.html" in state, state
    assert channel.read_bytes() == b"show23-pc channel 1\nstable\n"
    return {"launch": launch, "pointer": state, "unconfirmedBeta": NEW}


def run(ctx):
    ctx.record("M7-history-older-beta", "Lost pointers recover an older confirmed Beta after a damaged Stable.",
               lambda: recover(ctx, repacked=False))
    ctx.record("M7-history-repacked", "Stable excludes repacked and never-confirmed Betas during pointer recovery.",
               lambda: recover(ctx, repacked=True))
