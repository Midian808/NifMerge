"""Downloads the PyNifly test fixtures into tests/fixtures/pynifly/.

They are Skyrim game and mod assets (a Bethesda skeleton, XPMSE skeletons, a body mod mesh, a mod
FaceGen head) that this repository has no right to redistribute, so they are not committed. They are
fetched from PyNifly's public repository at a pinned commit and checked against SHA-256 hashes, so
every machine tests against identical bytes.

Usage: python tests/fetch_fixtures.py
Exit code 0 when every fixture is present and verified.
"""
import hashlib
import sys
import urllib.request
from pathlib import Path

COMMIT = "dc52d12c4e5b5ff96b6f2b42bee7fc0499accbe9"
BASE = f"https://raw.githubusercontent.com/BadDogSkyrim/PyNifly/{COMMIT}/"
DEST = Path(__file__).resolve().parent / "fixtures" / "pynifly"

# local name: (path in the PyNifly repository, SHA-256)
FIXTURES = {
    "skeleton_vanilla_le.nif": ("tests/tests/Skyrim/skeleton_vanilla.nif",
                                "0fc822dd02f36c2041e54a493915633711a0405ad1fd711287f82a382329234e"),
    "femalebody_1_le.nif": ("tests/tests/Skyrim/meshes/femalebody_1.nif",
                            "d807439266084cb60b0ac1f75d940dd157cea2d6b5b55ee1887059271346426c"),
    "facegen_se.nif": ("tests/tests/SkyrimSE/meshes/facegen.nif",
                       "d7043ca8ad0765f86b21ec2dd3caf7322a69dda4808360004a49908e339737eb"),
    "skeleton_female_xpmse.nif": ("io_scene_nifly/skeletons/Skyrim/skeleton_female.nif",
                                  "df4482313e9eb8662cfc1738db0ea3130a9c71ad3f1df328700a81776093140c"),
    "skeleton_male_xpmse.nif": ("io_scene_nifly/skeletons/Skyrim/skeleton.nif",
                                "31d50a0fe792df376d163beda0296e3aedfc0b115713b80e95e0e0e436ba6cc8"),
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    failed = 0
    for name, (path, want) in FIXTURES.items():
        target = DEST / name
        if target.exists() and sha256(target.read_bytes()) == want:
            print(f"ok       {name}")
            continue
        try:
            with urllib.request.urlopen(BASE + path, timeout=60) as r:
                data = r.read()
        except OSError as e:
            print(f"FAILED   {name}: cannot download {BASE + path}: {e}")
            failed += 1
            continue
        if sha256(data) != want:
            print(f"FAILED   {name}: downloaded file does not match the pinned SHA-256")
            failed += 1
            continue
        target.write_bytes(data)
        print(f"fetched  {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
