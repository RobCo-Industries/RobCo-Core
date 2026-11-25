# Changelog

## Unreleased

- The harness plugin removes a passing test's `tmp_path` when the test ends. A test that fails or errors in setup, call or teardown keeps its folder. Set `ROBCO_KEEP_TMP=1` to keep every folder. A project that sets `tmp_path_retention_policy` itself keeps its own value.
- `rimg_seal()` and `rimg_pack()` in `src/lib/rimg_seal.c`: the sealer and the packer in C. A host test runs every vector of `tests/vectors/rimg/vectors.ini` through them.
