"""Unit tests for tools/check_docs.py on small trees in a temp directory."""

import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import check_docs  # noqa: E402

HEADER = """\
#ifndef ROBCO_DEMO_H
#define ROBCO_DEMO_H
#define DEMO_MAX(a, b) ((a) > (b) ? (a) : (b));
typedef void (*demo_fn_t)(int);
/* int not_a_function(void); */
int demo_init(unsigned flags);
void demo_long(unsigned a,
               unsigned b) __attribute__((noreturn));
static inline int demo_inline(int x)
{
    return demo_init(x);
}
#endif
"""

API = """\
# API

```c
int demo_init(unsigned flags);
void demo_long(unsigned a,
               unsigned b) __attribute__((noreturn));
static inline int demo_inline(int x);
```

```c
static void helper(void)
{
    int rc = demo_init(0);
}
```
"""


def make_tree(tmp_path, api=API, header=HEADER, files=None):
    (tmp_path / "include" / "robco").mkdir(parents=True)
    (tmp_path / "include" / "robco" / "demo.h").write_text(header)
    (tmp_path / "docs" / "guide").mkdir(parents=True)
    (tmp_path / "docs" / "guide" / "API.md").write_text(api)
    for name, text in (files or {}).items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return str(tmp_path)


def test_clean_tree_passes(tmp_path, capsys):
    root = make_tree(tmp_path, files={"README.md": "See [API](docs/guide/API.md#api).\n"})
    assert check_docs.main(["--root", root]) == 0
    assert "ok" in capsys.readouterr().out


def test_header_parser():
    names = check_docs.declared_functions(HEADER, inline_bodies=True)
    assert names == {"demo_init", "demo_long", "demo_inline"}


def test_api_parser_ignores_calls_and_definitions():
    assert check_docs.api_functions(API) == {"demo_init", "demo_long", "demo_inline"}


def test_broken_link(tmp_path):
    root = make_tree(tmp_path, files={"README.md": "[x](docs/missing.md) and ![i](img.png)\n"})
    errors = check_docs.check_links(root, ["README.md"])
    assert len(errors) == 2
    assert "docs/missing.md" in errors[0]


def test_links_in_code_are_ignored(tmp_path):
    text = "`[x](nope.md)`\n\n```\n[y](nope.md)\n```\n[web](https://example.com/x)\n"
    root = make_tree(tmp_path, files={"README.md": text})
    assert check_docs.check_links(root, ["README.md"]) == []


def test_missing_anchor(tmp_path):
    doc = "# Title\n\n## Boot and entry (`boot.h`)\n\n## Same\n\n## Same\n\n```\n# not a heading\n```\n"
    links = ("[a](doc.md#boot-and-entry-booth) [b](doc.md#same-1) [c](#local)\n"
             "[d](doc.md#not-a-heading) [e](doc.md#nope)\n\n## Local\n")
    root = make_tree(tmp_path, files={"doc.md": doc, "links.md": links})
    errors = check_docs.check_links(root, ["links.md"])
    assert len(errors) == 2
    assert "#not-a-heading" in errors[0]
    assert "#nope" in errors[1]


def test_slugify_matches_github():
    assert check_docs.slugify("Boot and entry (`boot.h`)") == "boot-and-entry-booth"
    assert check_docs.slugify("Put it on an SD card") == "put-it-on-an-sd-card"
    assert check_docs.slugify("config.txt") == "configtxt"
    assert check_docs.slugify("CRC-32 (`crc32.h`)") == "crc-32-crc32h"
    assert check_docs.slugify("robco_main") == "robco_main"


def test_undocumented_function(tmp_path):
    api = API.replace("int demo_init(unsigned flags);\n", "")
    root = make_tree(tmp_path, api=api)
    errors = check_docs.check_api(root)
    assert errors == [f"{check_docs.API_MD}: demo_init() is declared in include/robco "
                      "but not documented"]


def test_stale_function(tmp_path):
    api = API.replace("```\n\n", "int demo_gone(void);\n```\n\n", 1)
    root = make_tree(tmp_path, api=api)
    errors = check_docs.check_api(root)
    assert errors == [f"{check_docs.API_MD}: demo_gone() is documented but no header "
                      "declares it"]


@pytest.mark.parametrize("text,what", [
    ("Written by Claude.\n", "banned word"),
    ("A milestone.\n", "banned word"),
    ("Two workers.\n", "banned word"),
    ("This is AI-generated.\n", "banned word"),
    ("One — two.\n", "em dash"),
    ("Done ✅\n", "emoji"),
])
def test_banned_content(tmp_path, text, what):
    root = make_tree(tmp_path, files={"README.md": text})
    errors = check_docs.check_words(root, ["README.md"])
    assert len(errors) == 1 and what in errors[0]


def test_framework_and_network_are_not_banned(tmp_path):
    root = make_tree(tmp_path, files={"README.md": "The firmware and the network work.\n"})
    assert check_docs.check_words(root, ["README.md"]) == []


def test_checked_commands(tmp_path):
    guide = ("Not run:\n\n```sh\nfalse\n```\n\n"
             "<!-- check: runs -->\n```sh\n# a comment\ntrue\nexit 3\n```\n")
    root = make_tree(tmp_path, files={check_docs.GUIDE_MD: guide})
    errors = check_docs.check_commands(root)
    assert len(errors) == 1
    assert "`exit 3` exited 3" in errors[0]


def test_main_reports_failures(tmp_path, capsys):
    root = make_tree(tmp_path, files={"README.md": "[x](gone.md)\n"})
    assert check_docs.main(["--root", root, "--no-commands"]) == 1
    assert "gone.md" in capsys.readouterr().err
