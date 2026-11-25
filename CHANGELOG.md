# Changelog

## Unreleased

- The harness plugin removes a passing test's `tmp_path` when the test ends. A failing test keeps its folder. Set `ROBCO_KEEP_TMP=1` to keep every folder. A project that sets `tmp_path_retention_policy` itself keeps its own value.
