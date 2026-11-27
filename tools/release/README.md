# Release tools

Standard library Python tools for a release of the suite. Run them with `python3 -I`. On an error they print an `::error::` line and exit with code 1. None of them makes a tag.

## cut_changelog.py

Turns the unreleased section of a changelog into a version section. It reads both `## Unreleased` and `## [Unreleased]`. It joins a `### ` group that appears twice into its first place and rewrites no entry. It refuses an empty unreleased section and a version that already has a section.

```sh
python3 -I tools/release/cut_changelog.py cut CHANGELOG.md --version 1.0.0 --date 2026-12-01
python3 -I tools/release/cut_changelog.py check CHANGELOG.md --version 1.0.0
python3 -I tools/release/cut_changelog.py section CHANGELOG.md --version 1.0.0
```

`check` passes when the unreleased section is empty and the next section is that version. `section` prints the version's section.

## notices.py

Joins the `LICENSE` of the six repositories and each licence file under `RobCo-Core/third_party/` into one file. A licence file is one whose name starts with `LICENSE`, `LICENCE` or `COPYING`. `--work` holds one checkout per repository, named as the repository. A missing licence file fails. The same inputs give the same bytes.

```sh
python3 -I tools/release/notices.py --work w --out NOTICES.txt
```

The font in `third_party/fixedsys-excelsior/` has no licence file, only `PROVENANCE`, so its terms are not in the output.
