# Vendored dependencies

| Directory | Upstream | Version | Licence | Local changes |
|---|---|---|---|---|
| `nifly/` | https://github.com/ousnius/nifly | commit `6cb15fb5da71527b33cea8ff7913be8790e1c633` | GPL-3.0 (`nifly/LICENSE`) | none. Upstream test data (`tests/input`, `tests/expected`, `tests/output`) removed. |
| `imgui/` | https://github.com/ocornut/imgui | tag `v1.91.8` (`dbb5eeaadffb6a3ba6a60de1290312e5802dba5a`) | MIT (`imgui/LICENSE.txt`) | none. Only the Win32 and DirectX 11 backends are kept; `examples/` and `docs/` removed. |

## Test fixtures (not vendored)

`tests/fixtures/pynifly/` is **not committed**. Its files are Skyrim game and mod assets, which
PyNifly's GPL-3.0 licence does not cover:
- `skeleton_vanilla_le.nif`: Bethesda's skeleton.
- `skeleton_female_xpmse.nif`, `skeleton_male_xpmse.nif`: the XPMSE skeletons.
- `femalebody_1_le.nif`: a body-mod mesh.
- `facegen_se.nif`: a FaceGen head from the YAS Lykaios mod.

`tests/fetch_fixtures.py` downloads them from https://github.com/BadDogSkyrim/PyNifly at commit
`dc52d12c4e5b5ff96b6f2b42bee7fc0499accbe9` and verifies each against a pinned SHA-256.
