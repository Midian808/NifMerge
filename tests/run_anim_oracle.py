"""Independent check of SkyAnim's .hkx decoder against a second implementation (tests/hkx_oracle, Python).

For every sample clip in samples/animationwork/{LE,SSE}:
  * the oracle decodes the clip itself, asserting that each spline block ends exactly at the offset the
    file records, that every reconstructed quaternion component is the largest one (the smallest-three
    encodings drop the largest), and that neighbouring blocks agree on the frame they share;
  * SkyAnim.exe dump writes its own decode, which must equal the oracle's for every frame, track and
    component (translation and scale within 1e-4 game units, rotation within 1e-6 per component).

Usage: python tests/run_anim_oracle.py      (set NIFMERGE_BUILD to test another build tree)
"""
import math
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent / "hkx_oracle"))
import hkxpack  # noqa: E402
import hkxobjs  # noqa: E402
import spline  # noqa: E402

BUILD = Path(os.environ.get("NIFMERGE_BUILD", ROOT / "build"))
SKYANIM = BUILD / "Release" / "SkyAnim.exe"
ANIM = ROOT / "samples" / "animationwork"
OUT = ROOT / ".tmp" / "anim_oracle"
CLIPS = ["LE/mt_idle.hkx", "LE/mt_walkforward.HKX", "LE/mt_runforward.HKX", "LE/bow_release.hkx",
         "SSE/mt_idle.hkx", "SSE/mt_walkforward.hkx", "SSE/mt_runforward.hkx"]

failures = 0


def check(ok, what):
    global failures
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def qangle(p, q):
    return math.degrees(2 * math.acos(min(1.0, abs(sum(a * b for a, b in zip(p, q))))))


def oracle_decode(path):
    """Decode with the oracle, checking its invariants. Returns (frames[frame][track], seam error deg, bad quats)."""
    bad = [0]
    place = spline.place

    def checked_place(three, missing, sign):
        q = place(three, missing, sign)
        if abs(q[missing]) + 1e-4 < max(abs(c) for c in three):
            bad[0] += 1
        return q

    spline.place = checked_place
    try:
        pf = hkxpack.Packfile(str(path))
        anim = hkxobjs.read_animation(pf, pf.find("hkaSplineCompressedAnimation")[0])
        blocks = []
        for b in range(anim["numBlocks"]):
            tracks, end = spline.parse_block(anim, b)
            check(end == anim["floatBlockOffsets"][b], f"{path.name}: oracle block {b} ends at the recorded offset")
            blocks.append(tracks)
    finally:
        spline.place = place
    fpb = anim["maxFramesPerBlock"] - 1
    seam = 0.0
    for b in range(anim["numBlocks"] - 1):
        for a, n in zip(blocks[b], blocks[b + 1]):
            seam = max(seam, qangle(spline.eval_spline(a[1], float(fpb), True), spline.eval_spline(n[1], 0.0, True)))
    frames = [spline.sample(anim, blocks, f) for f in range(anim["numFrames"])]
    return frames, seam, bad[0]


def main():
    check(SKYANIM.is_file(), f"SkyAnim.exe present ({SKYANIM})")
    if failures:
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    for rel in CLIPS:
        path = ANIM / rel
        check(path.is_file(), f"{rel}: fixture present")
        if not path.is_file():
            continue
        frames, seam, bad = oracle_decode(path)
        check(bad == 0, f"{rel}: every reconstructed quaternion component is the largest ({bad} violations)")
        check(seam < 0.2, f"{rel}: neighbouring blocks agree on their shared frame ({seam:.3f} deg)")

        tsv = OUT / (rel.replace("/", "_") + ".tsv")
        r = subprocess.run([str(SKYANIM), "dump", str(path), "--out", str(tsv)], capture_output=True, text=True)
        check(r.returncode == 0, f"{rel}: SkyAnim dump succeeds {r.stdout.strip()}")
        if r.returncode:
            continue
        worst_t = worst_q = worst_s = 0.0
        rows = 0
        with open(tsv, encoding="utf-8") as f:
            next(f)
            for line in f:
                v = line.split("\t")
                fr, tr = int(v[0]), int(v[1])
                t, q, s = (float(x) for x in v[2:5]), [float(x) for x in v[5:9]], [float(x) for x in v[9:12]]
                ot, oq, os_ = frames[fr][tr]
                sign = 1.0 if sum(a * b for a, b in zip(q, oq)) >= 0 else -1.0
                worst_t = max(worst_t, max(abs(a - b) for a, b in zip(t, ot)))
                worst_q = max(worst_q, max(abs(sign * a - b) for a, b in zip(q, oq)))
                worst_s = max(worst_s, max(abs(a - b) for a, b in zip(s, os_)))
                rows += 1
        expected = len(frames) * len(frames[0])
        check(rows == expected, f"{rel}: SkyAnim decoded {rows} transforms (oracle {expected})")
        check(worst_t < 1e-4 and worst_s < 1e-4 and worst_q < 1e-6,
              f"{rel}: SkyAnim equals the oracle (translation {worst_t:.2e}, rotation {worst_q:.2e}, scale {worst_s:.2e})")
    print(f"\n{failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
