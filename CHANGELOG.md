# Changelog

## Unreleased

- The nightly run, `.github/workflows/nightly.yml`: every night it builds the five siblings at their default heads on their own library pins, packs the terminal and Pip-Boy cards and runs the whole-chain tests on four boards. `tools/nightly/plan.py` resolves the commits and image tags into one record per built repository, which the matrices, the summary and `VERSIONS.txt` all read, so the Pip-Boy row names the library and tag of the Pip-OS head it builds on, `tools/nightly/junit_gate.py` fails a test job on a failure, an error, a skip or no passed test, and `tools/nightly/pack_card.py` refuses a card with Pi firmware. The README shows its badge.
- `tools/check_workflow_exprs.py` moves here from Pip-OS and adds a rule that every `uses:` names a full commit. `make check-workflows` runs it in CI and in each project that includes `mk/robco.mk`. The workflows pass values to scripts through `env:` and pin their actions by commit.
- `robco-ci.yml` marks the checkout and every submodule folder, at any depth, as safe for git in both jobs, then checks that git works in each. Before, git refused the checkout in the container with "dubious ownership".
- The harness plugin removes a passing test's `tmp_path` when the test ends. A test that fails or errors in setup, call or teardown keeps its folder. Set `ROBCO_KEEP_TMP=1` to keep every folder. A project that sets `tmp_path_retention_policy` itself keeps its own value.
- `rimg_seal()` and `rimg_pack()` in `src/lib/rimg_seal.c`: the sealer and the packer in C. A host test runs every vector of `tests/vectors/rimg/vectors.ini` through them.
- `tools/rimg.py` reads a PBM icon width or height as 1 to 9 ASCII digits, as the spec asks. It now refuses `+32`, `-32`, `3_2`, `0000000032` and a token with a vertical tab or a form feed with `io`, as the C sealer does. Seven new vectors check it.
- `.gitattributes` turns off end-of-line conversion for test vectors, golden images and byte-compared fixtures, so a Windows checkout with `core.autocrlf=true` keeps their bytes. `tests/tools/test_vector_eol.py` checks it for `tests/vectors/`.
