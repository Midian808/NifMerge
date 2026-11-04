"""SkyCreature GUI end-to-end test.

Drives the real SkyCreature GUI and checks that:
  * `SkyCreature gui <inputs> --go --exit-when-done` writes exactly what `SkyCreature export <inputs>` does (every
    FBX via fbx_summary, every .materials.json and .anim.json byte for byte) and exits 0,
  * files and folders dragged onto the window are routed by content: the skeleton .nif and .hkx fill the
    skeleton fields, the model .nif joins the parts, the dropped folder joins the animations; a real click on
    Go! then gives the same output as the CLI with --skeleton/--hkx/--part and that folder,
  * --exit-when-done returns 1 when a clip fails, while the model and the other clips are still written, and
    1 when the model cannot be made (Humanoid asked of the wolf), with nothing written,
  * a screenshot of the window after the drop run is saved (.tmp/creatureguitest/gui_drop_window.png).
It moves the mouse, so leave it alone while it runs.

Usage: python tests/run_creature_gui_test.py      (set NIFMERGE_BUILD to test another build tree)
"""
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("NIFMERGE_BUILD", ROOT / "build"))
EXE = BUILD / "Release" / "SkyCreature.exe"
SUMMARY = BUILD / "tests" / "Release" / "fbx_summary.exe"
INPUT = BUILD / "tests" / "Release" / "gui_input.exe"
WOLF = ROOT / "samples" / "Creatures" / "Wolf"
OUT = ROOT / ".tmp" / "creatureguitest"
user32 = ctypes.windll.user32
WM_CLOSE = 0x0010
CLIPS = ["mt_idle_wolf", "runforward", "trotforwardr", "walkforward_wolf", "walkforwardl_wolf", "wolf_idle_spice"]

failures = 0


def check(ok, what):
    global failures
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def make_input(folder, broken=False):
    """The wolf: skeleton files and the model at the top; the clips, the behavior file and another model
    variant (skipped: NIFs inside folders are not parts) in anims/."""
    shutil.rmtree(folder, ignore_errors=True)
    (folder / "anims").mkdir(parents=True)
    for f in ("skeleton.nif", "skeleton.hkx", "wolf.nif"):
        shutil.copy(WOLF / f, folder / f)
    for c in CLIPS:
        shutil.copy(WOLF / "anims" / (c + ".hkx"), folder / "anims" / (c + ".hkx"))
    shutil.copy(WOLF / "wolf.hkx", folder / "anims" / "wolf.hkx")
    shutil.copy(WOLF / "wolfblack.nif", folder / "anims" / "wolfblack.nif")
    if broken:
        data = (WOLF / "anims" / "runforward.hkx").read_bytes()
        (folder / "anims" / "broken.hkx").write_bytes(data[: len(data) // 2])


def outputs(folder):
    """Every exported file below folder: relative path -> comparable content (FBX summary or bytes)."""
    result = {}
    for p in sorted(folder.rglob("*")):
        if p.suffix.lower() == ".fbx":
            result[p.relative_to(folder).as_posix()] = subprocess.run([str(SUMMARY), str(p)], capture_output=True, text=True,
                                                                     check=True).stdout
        elif p.name.endswith(".anim.json") or p.name.endswith(".materials.json"):
            result[p.relative_to(folder).as_posix()] = p.read_bytes()
    return result


def window_of(pid):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, cls, 64)
        if owner.value == pid and cls.value == "SkyCreatureWindow":
            found.append(hwnd)
        return True
    user32.EnumWindows(visit, 0)
    return found[0] if found else None


def screenshot(hwnd, path):
    rect = wintypes.RECT()
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    ps = (
        "Add-Type -AssemblyName System.Drawing;"
        f"$b = New-Object System.Drawing.Bitmap({rect.right - rect.left}, {rect.bottom - rect.top});"
        "$g = [System.Drawing.Graphics]::FromImage($b);"
        f"$g.CopyFromScreen({rect.left}, {rect.top}, 0, 0, $b.Size);"
        f"$b.Save('{path}', [System.Drawing.Imaging.ImageFormat]::Png)"
    )
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True)


def drop_run(folder, cli_out):
    out = OUT / "drop_out"
    shutil.rmtree(out, ignore_errors=True)
    layout_file = OUT / "layout.json"
    layout_file.unlink(missing_ok=True)
    proc = subprocess.Popen([str(EXE), "gui", "--out", str(out), "--write-layout", str(layout_file)])
    deadline = time.time() + 30
    while time.time() < deadline and not (layout_file.exists() and window_of(proc.pid)):
        time.sleep(0.25)
    hwnd = window_of(proc.pid)
    check(bool(hwnd) and layout_file.exists(), "drop: GUI window open and its layout written")
    if not hwnd or not layout_file.exists():
        proc.kill()
        return
    time.sleep(0.5)
    layout = json.loads(layout_file.read_text())
    x = (layout["go"][0] + layout["go"][2]) // 2
    y = (layout["parts"][0] + layout["parts"][1]) // 2

    def drop(files, what):
        r = subprocess.run([str(INPUT), "drop", str(hwnd), str(x), str(y)] + [str(f) for f in files], capture_output=True, text=True)
        check(r.returncode == 0, f"drop: {what} dropped ({r.stderr.strip() or 'ok'})")
        time.sleep(0.5)

    drop([folder / "skeleton.nif", folder / "skeleton.hkx", folder / "wolf.nif"], "the skeleton .nif and .hkx and the model")
    drop([folder / "anims"], "the animations folder")
    go = layout["go"]
    r = subprocess.run([str(INPUT), "click", str(hwnd), str((go[0] + go[2]) // 2), str((go[1] + go[3]) // 2)])
    check(r.returncode == 0, "drop: Go! clicked")
    report = out / "anims" / "batch_report.json"
    deadline = time.time() + 120
    while time.time() < deadline and not report.exists():
        time.sleep(0.25)
    check(report.exists(), "drop: Go! wrote the output")
    time.sleep(1.0)
    screenshot(hwnd, OUT / "gui_drop_window.png")
    print("INFO screenshot", OUT / "gui_drop_window.png")
    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    check(proc.wait(timeout=30) == 0, "drop: GUI closes cleanly")
    if report.exists():
        check(outputs(out) == outputs(cli_out), "drop: every FBX and sidecar identical to the CLI's")


def main():
    for f in (EXE, SUMMARY, INPUT):
        if not f.exists():
            sys.exit(f"Build first: {f} missing")
    OUT.mkdir(parents=True, exist_ok=True)
    folder = OUT / "in"
    make_input(folder)
    inputs = [str(folder / "skeleton.nif"), str(folder / "wolf.nif"), str(folder / "anims")]

    # CLI reference runs: by content, and with everything named explicitly (what the drop run does).
    cli_out, cli_named = OUT / "cli_out", OUT / "cli_named_out"
    for d in (cli_out, cli_named):
        shutil.rmtree(d, ignore_errors=True)
    check(subprocess.run([str(EXE), "export"] + inputs + ["--out", str(cli_out)]).returncode == 0, "CLI export succeeds")
    check(subprocess.run([str(EXE), "export", "--skeleton", str(folder / "skeleton.nif"), "--hkx", str(folder / "skeleton.hkx"),
                          "--part", str(folder / "wolf.nif"), str(folder / "anims"), "--out", str(cli_named)]).returncode == 0,
          "CLI export with everything named succeeds")
    expected = outputs(cli_out)
    want = {"wolf.fbx", "wolf.materials.json"} | {f"anims/{c}.fbx" for c in CLIPS} | {f"anims/{c}.anim.json" for c in CLIPS}
    check(set(expected) == want, f"CLI wrote the model and 6 clips ({sorted(expected)})")
    check(json.loads((cli_out / "wolf.materials.json").read_text())["rig"] == "generic", "the wolf is Generic")

    # GUI with --go: same inputs, same output, exit code 0.
    gui_out = OUT / "gui_out"
    shutil.rmtree(gui_out, ignore_errors=True)
    code = subprocess.run([str(EXE), "gui"] + inputs + ["--out", str(gui_out), "--go", "--exit-when-done"]).returncode
    check(code == 0, f"GUI --go --exit-when-done exits 0 (got {code})")
    check(outputs(gui_out) == expected, "GUI output identical to the CLI's")

    # Real drag and drop, then a real click on Go!.
    drop_run(folder, cli_named)

    # A failing clip: exit code 1, the model and the rest still written.
    broken = OUT / "in_broken"
    make_input(broken, broken=True)
    broken_out = OUT / "broken_out"
    shutil.rmtree(broken_out, ignore_errors=True)
    code = subprocess.run([str(EXE), "gui", str(broken / "skeleton.nif"), str(broken / "wolf.nif"), str(broken / "anims"),
                           "--out", str(broken_out), "--go", "--exit-when-done"]).returncode
    check(code == 1, f"GUI exits 1 when a clip fails (got {code})")
    report = json.loads((broken_out / "anims" / "batch_report.json").read_text())
    check(report["exported"] == 6 and report["failed"] == 1 and (broken_out / "wolf.fbx").exists(),
          f"the model and the other clips still written ({report['exported']} / {report['failed']})")

    # A model that cannot be made: exit code 1, nothing written.
    refused_out = OUT / "refused_out"
    shutil.rmtree(refused_out, ignore_errors=True)
    code = subprocess.run([str(EXE), "gui"] + inputs + ["--rig", "humanoid", "--out", str(refused_out), "--go",
                                                         "--exit-when-done"]).returncode
    check(code == 1 and not refused_out.exists(), f"Humanoid asked of the wolf: exit 1, nothing written (got {code})")

    print(f"\n{failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
