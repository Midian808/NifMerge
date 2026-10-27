"""SkyAnim GUI end-to-end test (milestone A6).

Drives the real SkyAnim GUI and checks that:
  * `SkyAnim gui <folder> --go --exit-when-done` exports exactly what `SkyAnim export <folder>` does (every FBX
    via fbx_summary, every .anim.json byte for byte) and exits 0,
  * files and folders dragged onto the window are routed by content: the skeleton .nif and .hkx dropped as
    files fill the skeleton fields, the dropped folder joins the inputs; a real click on Go! then gives the
    same output as the CLI with --skeleton/--hkx and that folder,
  * --exit-when-done returns 1 when a clip fails, while the other clips are still exported,
  * a screenshot of the window after the drop run is saved (.tmp/animguitest/gui_drop_window.png).
It moves the mouse, so leave it alone while it runs.

Usage: python tests/run_anim_gui_test.py      (set NIFMERGE_BUILD to test another build tree)
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
EXE = BUILD / "Release" / "SkyAnim.exe"
SUMMARY = BUILD / "tests" / "Release" / "fbx_summary.exe"
INPUT = BUILD / "tests" / "Release" / "gui_input.exe"
ANIM = ROOT / "samples" / "animationwork"
OUT = ROOT / ".tmp" / "animguitest"
user32 = ctypes.windll.user32
WM_CLOSE = 0x0010

failures = 0


def check(ok, what):
    global failures
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def make_input(folder, broken=False):
    """A dropped folder: SE skeleton at the top, SE and LE clips in subfolders, the character file."""
    shutil.rmtree(folder, ignore_errors=True)
    (folder / "se").mkdir(parents=True)
    (folder / "le").mkdir()
    (folder / "characters").mkdir()
    for f in ("skeleton_female.nif", "skeleton_female.hkx"):
        shutil.copy(ANIM / "SSE" / f, folder / f)
    for f in ("mt_idle.hkx", "mt_walkforward.hkx", "mt_runforward.hkx"):
        shutil.copy(ANIM / "SSE" / f, folder / "se" / f)
    shutil.copy(ANIM / "LE" / "bow_release.hkx", folder / "le" / "bow_release.hkx")
    shutil.copy(ANIM / "LE" / "defaultfemale.hkx", folder / "characters" / "defaultfemale.hkx")
    if broken:
        data = (ANIM / "SSE" / "mt_walkforward.hkx").read_bytes()
        (folder / "se" / "broken.hkx").write_bytes(data[: len(data) // 2])


def outputs(folder):
    """Every exported file below folder: relative path -> comparable content (FBX summary or bytes)."""
    result = {}
    for p in sorted(folder.rglob("*")):
        if p.suffix.lower() == ".fbx":
            result[p.relative_to(folder).as_posix()] = subprocess.run([str(SUMMARY), str(p)], capture_output=True, text=True,
                                                                     check=True).stdout
        elif p.name.endswith(".anim.json"):
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
        if owner.value == pid and cls.value == "SkyAnimWindow":
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
    y = (layout["inputs"][0] + layout["inputs"][1]) // 2

    def drop(files, what):
        r = subprocess.run([str(INPUT), "drop", str(hwnd), str(x), str(y)] + [str(f) for f in files], capture_output=True, text=True)
        check(r.returncode == 0, f"drop: {what} dropped ({r.stderr.strip() or 'ok'})")
        time.sleep(0.5)

    drop([folder / "skeleton_female.nif", folder / "skeleton_female.hkx"], "the skeleton .nif and .hkx")
    drop([folder], "the animations folder")
    go = layout["go"]
    r = subprocess.run([str(INPUT), "click", str(hwnd), str((go[0] + go[2]) // 2), str((go[1] + go[3]) // 2)])
    check(r.returncode == 0, "drop: Go! clicked")
    report = out / "batch_report.json"
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
    folder = OUT / "in"
    make_input(folder)

    # CLI reference runs: by content, and with the skeleton named explicitly (what the drop run does).
    cli_out, cli_named = OUT / "cli_out", OUT / "cli_named_out"
    for d in (cli_out, cli_named):
        shutil.rmtree(d, ignore_errors=True)
    check(subprocess.run([str(EXE), "export", str(folder), "--out", str(cli_out)]).returncode == 0, "CLI export succeeds")
    check(subprocess.run([str(EXE), "export", "--skeleton", str(folder / "skeleton_female.nif"), "--hkx", str(folder / "skeleton_female.hkx"),
                          str(folder), "--out", str(cli_named)]).returncode == 0, "CLI export with a named skeleton succeeds")
    expected = outputs(cli_out)
    check(len([k for k in expected if k.endswith(".fbx")]) == 4, f"CLI wrote 4 clips ({sorted(expected)})")

    # GUI with --go: same inputs, same output, exit code 0.
    gui_out = OUT / "gui_out"
    shutil.rmtree(gui_out, ignore_errors=True)
    code = subprocess.run([str(EXE), "gui", str(folder), "--out", str(gui_out), "--go", "--exit-when-done"]).returncode
    check(code == 0, f"GUI --go --exit-when-done exits 0 (got {code})")
    check(outputs(gui_out) == expected, "GUI output identical to the CLI's")

    # Real drag and drop, then a real click on Go!.
    drop_run(folder, cli_named)

    # A failing clip: exit code 1, the rest still exported.
    broken = OUT / "in_broken"
    make_input(broken, broken=True)
    broken_out = OUT / "broken_out"
    shutil.rmtree(broken_out, ignore_errors=True)
    code = subprocess.run([str(EXE), "gui", str(broken), "--out", str(broken_out), "--go", "--exit-when-done"]).returncode
    check(code == 1, f"GUI exits 1 when a clip fails (got {code})")
    report = json.loads((broken_out / "batch_report.json").read_text())
    check(report["exported"] == 4 and report["failed"] == 1, f"the other clips still exported ({report['exported']} / {report['failed']})")

    print(f"\n{failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
