# Suite releases

A suite release names one commit and one version for each of the six repositories. Its manifest lives here as `releases/suite-<YYYY.MM>.json`. A second release in the same month is `suite-<YYYY.MM>.1`, then `.2`, and so on.

[`tools/release/manifest.py`](../tools/release/manifest.py) writes and checks these files. It uses the Python standard library only.

## The manifest

```json
{
  "suite": "suite-2026.12",
  "repositories": {
    "RobCo-Core": {"version": "0.1.0", "commit": "<40 hex digits>"},
    "RETROS-BIOS": {"version": "0.1.0", "commit": "<40 hex digits>"},
    "MFBootAgent": {"version": "0.1.0", "commit": "<40 hex digits>"},
    "UOS": {"version": "0.1.0", "commit": "<40 hex digits>"},
    "Pip-OS": {"version": "7.1.0", "commit": "<40 hex digits>"},
    "Deitrix": {"version": "0.1.0", "commit": "<40 hex digits>"}
  }
}
```

- The file has the keys `suite` and `repositories` and nothing else.
- `suite` is `suite-YYYY.MM` or `suite-YYYY.MM.N`.
- All six repositories are there, and no other one.
- Each entry has `version`, as `MAJOR.MINOR.PATCH`, and `commit`, the full 40-digit commit. Nothing else.
- The RobCo-Core commit is the library commit the stages pin. The manifest itself sits on a later commit.

## The rules

`check` reads the six checkouts under a work folder, one folder per repository, named as the repository. It reads every file at the manifest's commit with git, never from the working tree, so an edit that is not committed changes nothing. A file missing at that commit is an error that names the repository, the commit and the path. It checks every rule, prints one `::error::` line per broken rule and only then exits with 1.

- The manifest has the shape above.
- Each checkout holds the manifest's commit. It does not have to be checked out.
- Each commit is on its repository's default branch: `main` for RobCo-Core and RETROS-BIOS, `master` for the others.
- RETROS-BIOS, MFBootAgent, UOS and Pip-OS pin the manifest's RobCo-Core commit.
- Deitrix pins the manifest's Pip-OS commit.
- The version in code equals the manifest's version. RETROS-BIOS reads `RETROS_VERSION_STR` in `src/robco_config.h`, MFBootAgent `MFB_VERSION_STR` in `src/robco_config.h`, UOS `UOS_VERSION_STR` in `src/kernel/robco_config.h`, Pip-OS the three `PIPOS_VERSION_` lines in `kernel/robco_config.h`, and Deitrix `VERSION :=` in its `Makefile`. RobCo-Core has no version in code.
- Each `CHANGELOG.md` has an empty `## Unreleased` or `## [Unreleased]` section, and the next section is the manifest's version, as `## [0.1.0] - date` or `## 0.1.0`. `--rehearsal` skips this rule and no other.
- With `--tag NAME`, the tag equals the manifest's `suite`.

A `v` tag of a repository is checked by `remote-tags`, not by `check`.

## The commands

```sh
python3 -I tools/release/manifest.py new --suite suite-2026.12 --work w --out releases/suite-2026.12.json
python3 -I tools/release/manifest.py check --manifest F --work w [--rehearsal] [--tag NAME] [--github-output F]
python3 -I tools/release/manifest.py verify-night --manifest F --versions VERSIONS.txt
python3 -I tools/release/manifest.py remote-tags --manifest F [--require] [--remote REPO=URL ...]
python3 -I tools/release/manifest.py tags --manifest F
```

- `new` writes a manifest from the checkouts under `w/`: each one's `HEAD` commit and the version in code at that commit. RobCo-Core takes the newest version section of its changelog, or `0.1.0` when it has none. It writes `--out` whole or not at all.
- `check` runs the rules above. With `--github-output` it writes `suite` and the six commits as `core_sha`, `retros_sha`, `mfboot_sha`, `uos_sha`, `pipos_sha` and `deitrix_sha`.
- `verify-night` reads the `VERSIONS.txt` of a nightly run. Each repository's 12-digit commit must start the manifest's commit, and its Note column must be empty, which means no pin differed.
- `remote-tags` asks each repository for its `v<version>` tag with `git ls-remote`. It is the only command that talks to a remote. A tag on another commit fails. A missing tag is printed and passes, unless `--require` is given. `--remote REPO=URL` replaces a repository's address, which is `https://github.com/RobCo-Industries/<name>.git` by default.
- `tags` prints the commands that make the six `v` tags, push them, then make and push the suite tag in RobCo-Core. It runs nothing. It does not ask a remote, so it prints a line for a tag that exists already: skip the lines of the tags `remote-tags` reports as present. The suite tag line holds `MANIFEST_COMMIT` in place of the commit that holds the manifest, since the tool cannot know it.

Every command exits 0 on success and 1 with `::error::` lines on any failure. An output file that cannot be written is such a failure, and its line names the path.
