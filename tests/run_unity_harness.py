"""Unity batch-mode verification of NifMerge output (milestone M3).

For each Unity editor given, this:
  1. creates (once) a scratch Built-in pipeline project under .tmp/unity/<version>,
  2. references the com.nifmerge.importer package from this repo,
  3. runs NifMerge.exe on the user's sample set into Assets/NifMergeTest/Character.fbx, and again
     wearing the sample outfit (dress, boots, thong) into Assets/NifMergeTest/Outfit.fbx,
  4. writes small DDS test textures at some of the paths the sidecar references,
  5. opens the project in batch mode (the automatic import path a user hits) and runs
     NifMergeHarness.Run, which checks the imported result and renders two screenshots.

Usage: python tests/run_unity_harness.py [--unity <Unity.exe> ...]
Exit code 0 only when every editor passes.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("NIFMERGE_BUILD", ROOT / "build"))  # build tree to test
NIFMERGE = BUILD / "Release" / "NifMerge.exe"
SAMPLES = ROOT / "samples"
DEFAULT_EDITORS = [
    r"D:\UnityHubFuckery\6000.0.68f1\Editor\Unity.exe",
    r"D:\UnityHubFuckery\2022.3.61f1\Editor\Unity.exe",
]


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def write_dds(path: Path, rgba, fourcc: bytes, size=8):
    """A solid-colour BC1 (DXT1) or BC3 (DXT5) texture, no mipmaps."""
    r, g, b, a = rgba
    colour = struct.pack("<HHI", rgb565(r, g, b), rgb565(r, g, b), 0)
    block = colour if fourcc == b"DXT1" else struct.pack("<BB6s", a, a, bytes(6)) + colour
    blocks = (size // 4) * (size // 4)
    header = struct.pack(
        "<4sIIIIIII44sIIIIIIIIIIIII",
        b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, size, size, blocks * len(block), 0, 0, bytes(44),
        32, 0x4, struct.unpack("<I", fourcc)[0], 0, 0, 0, 0, 0,
        0x1000, 0, 0, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + block * blocks)


def texture_rel(nif_path: str) -> str:
    rel = nif_path.replace("\\", "/").lstrip("/")
    if rel.lower().startswith("textures/"):
        rel = rel[len("textures/"):]
    return rel


def role(materials, name):
    """Finds a slot by what it is, never by the sample's own shape names. Must match the Roles class
    in tests/unity/NifMergeHarness.cs."""
    tests = {
        # the skin-tinted slot in body slot 32: the character's own body, or an outfit's body copy
        "body": lambda m: m["shaderType"] == 5 and 32 in m["slots"],
        "head": lambda m: m["shaderType"] == 4,  # FaceGen face shader
        "hair": lambda m: m["shaderType"] == 6 and bool(m["alphaFlags"] & (1 << 9)) and m["alphaProperty"]
                          and len(m["textures"]) > 1 and bool(m["textures"][1]),
    }
    found = next((m for m in materials if tests[name](m)), None)
    if found is None:
        sys.exit(f"No {name} slot in the sidecar: the sample set needs a body, a FaceGen head and alpha-tested hair")
    return found


def run(cmd, log=None):
    print(">", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], stdout=log, stderr=subprocess.STDOUT if log else None).returncode


def harness(unity: Path) -> bool:
    version = unity.parent.parent.name
    work = ROOT / ".tmp" / "unity" / version
    project = work / "Project"
    out = work / "out"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.iterdir():  # clear the previous run's results (the folder itself may be in use)
        if old.is_file():
            old.unlink()

    if not (project / "ProjectSettings").exists():
        with open(work / "create.log", "w") as log:
            if run([unity, "-batchmode", "-quit", "-nographics", "-createProject", project], log) != 0:
                print(f"[{version}] project creation failed, see {work / 'create.log'}")
                return False

    manifest_path = project / "Packages" / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["dependencies"]["com.nifmerge.importer"] = "file:" + (ROOT / "unity" / "com.nifmerge.importer").as_posix()
    manifest_path.write_text(json.dumps(manifest, indent=2))

    assets = project / "Assets"
    for d in ("NifMergeTest", "SkyrimTextures", "Editor"):
        shutil.rmtree(assets / d, ignore_errors=True)
        meta = assets / (d + ".meta")
        if meta.exists():
            meta.unlink()
    (assets / "Editor").mkdir(parents=True)
    shutil.copy(ROOT / "tests" / "unity" / "NifMergeHarness.cs", assets / "Editor" / "NifMergeHarness.cs")
    (project / "ProjectSettings" / "NifMergeSettings.asset").unlink(missing_ok=True)

    fbx = assets / "NifMergeTest" / "Character.fbx"
    outfit_fbx = assets / "NifMergeTest" / "Outfit.fbx"  # the same character wearing the sample outfit
    parts = ["femalebody_0.nif", "femalehands_0.nif", "femalefeet_0.nif", "Head.nif"]
    outfit = ["clothing/Dress/01_LatexDress_0.nif", "clothing/Boots/02_HighBoots_0.nif", "clothing/Thong/03_String_0.nif"]
    for out_fbx, wear in ((fbx, []), (outfit_fbx, outfit)):
        cmd = [NIFMERGE, "merge", "--skeleton", SAMPLES / "skeleton_female.nif", "--weight", "50",
               "--tri", SAMPLES / "trifiles", "--out", out_fbx]
        for p in parts:
            cmd += ["--part", SAMPLES / p]
        for p in wear:
            cmd += ["--wear", SAMPLES / p]
        if run(cmd) != 0:
            print(f"[{version}] NifMerge failed")
            return False

    sidecar = json.loads(fbx.with_name("Character.materials.json").read_text())
    mats = sidecar["materials"]
    body, hair, head = role(mats, "body"), role(mats, "hair"), role(mats, "head")
    tex = assets / "SkyrimTextures"
    write_dds(tex / texture_rel(body["textures"][0]), (200, 150, 120, 255), b"DXT1")
    write_dds(tex / texture_rel(hair["textures"][0]), (128, 128, 128, 255), b"DXT5")
    write_dds(tex / texture_rel(hair["textures"][1]), (128, 64, 255, 100), b"DXT5")
    write_dds(tex / texture_rel(head["textures"][6]), (128, 128, 128, 255), b"DXT1")
    # The outfit's items: a diffuse and a tangent-space normal map each.
    outfit_mats = json.loads(outfit_fbx.with_name("Outfit.materials.json").read_text())["materials"]
    for m in outfit_mats:
        if m["role"] == "outfit" and not m["modelSpaceNormals"]:
            write_dds(tex / texture_rel(m["textures"][0]), (40, 40, 48, 255), b"DXT1")
            write_dds(tex / texture_rel(m["textures"][1]), (128, 128, 255, 255), b"DXT5")
    # The user's real model-space normal maps: every _msn either sidecar names that samples/ holds.
    in_samples = {p.name.lower(): p for p in SAMPLES.glob("*.dds")}
    for m in mats + outfit_mats:
        if m["modelSpaceNormals"] and len(m["textures"]) > 1 and m["textures"][1]:
            src = in_samples.get(Path(m["textures"][1].replace("\\", "/")).name.lower())
            if src:
                dst = tex / texture_rel(m["textures"][1])
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(src, dst)

    log_path = work / "harness.log"
    env_cmd = [unity, "-batchmode", "-projectPath", project, "-executeMethod", "NifMergeHarness.Run",
               "-logFile", log_path]
    env = dict(os.environ, NIFMERGE_HARNESS_OUT=str(out))
    print(">", " ".join(str(c) for c in env_cmd), flush=True)
    code = subprocess.run([str(c) for c in env_cmd], env=env).returncode
    result = out / "harness_result.txt"
    if result.exists():
        print(result.read_text())
    else:
        print(f"[{version}] no result file; see {log_path}")
    print(f"[{version}] exit code {code}; log {log_path}")
    return code == 0 and result.exists()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--unity", action="append", help="Unity.exe to test with (repeatable)")
    args = ap.parse_args()
    if not NIFMERGE.exists():
        sys.exit(f"Build first: {NIFMERGE} missing")
    editors = [Path(u) for u in (args.unity or DEFAULT_EDITORS)]
    results = {}
    for u in editors:
        if not u.exists():
            print(f"missing editor {u}")
            results[str(u)] = False
            continue
        results[str(u)] = harness(u)
    for u, ok in results.items():
        print(("PASS " if ok else "FAIL ") + u)
    sys.exit(0 if all(results.values()) else 1)


if __name__ == "__main__":
    main()
