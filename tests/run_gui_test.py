"""GUI end-to-end test (milestone M4).

Drives the real GUI: launches `NifMerge gui ... --go`, which fills the window and presses Go! on the
first frame (the same StartRun the button calls). Then it checks that:
  * the GUI output is identical to the CLI output (FBX via fbx_summary, sidecar byte for byte),
  * a screenshot of the window after the run is saved (.tmp/guitest/gui_window.png),
  * --exit-when-done returns 0 on success and 1 when the run fails.
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("NIFMERGE_BUILD", ROOT / "build"))  # build tree to test
EXE = BUILD / "Release" / "NifMerge.exe"
SUMMARY = BUILD / "tests" / "Release" / "fbx_summary.exe"
S = ROOT / "samples"
OUT = ROOT / ".tmp" / "guitest"
PARTS = ["femalebody_0.nif", "femalehands_0.nif", "femalefeet_0.nif", "Head.nif"]
user32 = ctypes.windll.user32
WM_CLOSE = 0x0010

failures = 0


def check(ok, what):
    global failures
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def args_for(out_fbx, skeleton="skeleton_female.nif"):
    a = ["--skeleton", str(S / skeleton), "--weight", "50", "--out", str(out_fbx)]
    for p in PARTS:
        a += ["--part", str(S / p)]
    return a


def summary(fbx):
    return subprocess.run([str(SUMMARY), str(fbx)], capture_output=True, text=True, check=True).stdout


def window_of(pid):
    """The NifMerge window owned by this process (never another NifMerge the user has open)."""
    found = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, cls, 64)
        if owner.value == pid and cls.value == "NifMergeWindow":
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


def main():
    for f in (EXE, SUMMARY):
        if not f.exists():
            sys.exit(f"Build first: {f} missing")
    (OUT / "cli").mkdir(parents=True, exist_ok=True)
    (OUT / "gui").mkdir(parents=True, exist_ok=True)
    cli_fbx, gui_fbx = OUT / "cli" / "Character.fbx", OUT / "gui" / "Character.fbx"
    for f in (cli_fbx, gui_fbx, gui_fbx.with_name("Character.materials.json")):
        f.unlink(missing_ok=True)

    check(subprocess.run([str(EXE), "merge"] + args_for(cli_fbx)).returncode == 0, "CLI merge succeeds")

    # Interactive-style run: window stays open after Go! so it can be photographed.
    proc = subprocess.Popen([str(EXE), "gui"] + args_for(gui_fbx) + ["--go"])
    sidecar = gui_fbx.with_name("Character.materials.json")
    deadline = time.time() + 120
    while time.time() < deadline and not sidecar.exists():
        time.sleep(0.25)
    check(sidecar.exists(), "GUI Go! wrote the output")
    time.sleep(1.0)  # let the log pane show the finished run
    hwnd = window_of(proc.pid)
    check(bool(hwnd), "GUI window found")
    if hwnd:
        screenshot(hwnd, OUT / "gui_window.png")
        print("INFO screenshot", OUT / "gui_window.png")
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    check(proc.wait(timeout=30) == 0, "GUI closes cleanly")

    check(summary(cli_fbx) == summary(gui_fbx), "GUI FBX identical to CLI FBX (canonical summary)")
    check(cli_fbx.with_name("Character.materials.json").read_bytes() == sidecar.read_bytes(),
          "GUI sidecar identical to CLI sidecar")

    ok = subprocess.run([str(EXE), "gui"] + args_for(OUT / "gui" / "Again.fbx") + ["--go", "--exit-when-done"])
    check(ok.returncode == 0, "--exit-when-done: exit code 0 after a successful run")
    bad = subprocess.run([str(EXE), "gui"] + args_for(OUT / "gui" / "Bad.fbx", skeleton="Head.nif") +
                         ["--go", "--exit-when-done"])
    check(bad.returncode == 1, f"--exit-when-done: exit code 1 after a failed run (got {bad.returncode})")

    print(f"\n{failures} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
