# Changelog

## Unreleased

- The harness plugin removes a passing test's `tmp_path` when the test ends. A test that fails or errors in setup, call or teardown keeps its folder. Set `ROBCO_KEEP_TMP=1` to keep every folder. A project that sets `tmp_path_retention_policy` itself keeps its own value.
- `rimg_seal()` and `rimg_pack()` in `src/lib/rimg_seal.c`: the sealer and the packer in C. A host test runs every vector of `tests/vectors/rimg/vectors.ini` through them.
- `tools/rimg.py` reads a PBM icon width or height as 1 to 9 ASCII digits, as the spec asks. It now refuses `+32`, `-32`, `3_2`, `0000000032` and a token with a vertical tab or a form feed with `io`, as the C sealer does. Seven new vectors check it.
- `.gitattributes` turns off end-of-line conversion for test vectors, golden images and byte-compared fixtures, so a Windows checkout with `core.autocrlf=true` keeps their bytes. `tests/tools/test_vector_eol.py` checks it for `tests/vectors/`.
