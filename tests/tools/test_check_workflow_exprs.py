"""Tests for tools/check_workflow_exprs.py: the expression rule and the pin rule.

Written with unittest so they run with pytest and without it.
"""

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "check_workflow_exprs.py")

_spec = importlib.util.spec_from_file_location("check_workflow_exprs", TOOL)
cwe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cwe)


def lines(text):
    return [num for num, _ in cwe.findings(text)]


MUST_FAIL = {
    "run": '      - name: x\n        run: echo "${{ github.ref_name }}"\n',
    "space before colon": "        run : echo ${{ github.ref_name }}\n",
    "dash run": "      - run: echo ${{ github.ref_name }}\n",
    "block ref": "        run: |\n          echo ${{ github.ref }}\n",
    "block bracket": "        run: |\n          echo ${{ github['ref_name'] }}\n",
    "block inputs": "        run: >-\n          echo ${{ inputs.x }}\n",
    "block matrix": "        run: |2\n          echo ${{ matrix.x }}\n",
    "block secrets": "        run: |\n          echo ok\n\n          X: ${{ secrets.x }}\n",
    "block under other key": "        body: |\n          ${{ github.ref }}\n",
    "split": "        X: ${{ github.ref\n          }}\n",
    "flow mapping": "      - {run: echo ${{ x }}}\n",
    "quoted key": '        "run": echo ${{ x }}\n',
    "comment": "        # echo ${{ github.ref_name }}\n",
    "script": "          script: console.log('${{ x }}')\n",
    "shell": "        shell: ${{ x }} {0}\n",
    "args": "          args: ${{ x }}\n",
    "entrypoint upper": "          Entrypoint: ${{ x }}\n",
    "anchor before block": "        run: &script |\n          X: ${{ x }}\n",
    "alias": "        steps: *script\n        X: ${{ x }}\n",
    "merge key": "        <<: {}\n        X: ${{ x }}\n",
    "flow sequence": '    steps: [{run: "echo ${{ x }}"}]\n',
    "flow mapping value": "        with: {path: ${{ x }}}\n",
    "quoted multi-line run": (
        '        run: "echo a\n          X: b ${{ x }}"\n'
    ),
    "quoted value ends": '        env:\n          A: "x\n          B: ${{ x }}"\n',
    "tag before block": (
        "        run: !!str |\n          env:\n            X: ${{ x }}\n"
    ),
    "plain continuation": "        run: echo a\n          X: ${{ x }}\n",
    "alias uses": "        uses: *step\n",
    "merge alias": "        <<: *base\n",
    "anchor env": "        env: &e\n          A: b\n",
}

MUST_PASS = {
    "env entry": "        env:\n          TAG: ${{ github.ref_name }}\n",
    "dash run then env": (
        "      - run: echo ok\n        env:\n          TAG: ${{ github.ref_name }}\n"
    ),
    "if": "        if: ${{ github.event_name == 'push' }}\n",
    "group": "  group: sdk-package-${{ github.ref }}\n",
    "with": "        with:\n          path: build/${{ matrix.board }}\n",
    "quoted text no expr": '        name: "Example {run: echo hi}"\n',
    "block ends then env": (
        "      - run: |\n          echo ok\n        env:\n          TAG: ${{ x }}\n"
    ),
    "quotes inside expr": "          TAG: ${{ a && b || '' }}\n",
    "sibling if": "      - name: x\n        if: ${{ x }}\n",
    "concurrency group": "concurrency:\n  group: g-${{ github.ref }}\n",
    "shell globs in block": (
        "      - run: |\n"
        "          case \"$out\" in\n"
        "            *error*) exit 1 ;;\n"
        "          esac\n"
        "          rm *foo\n"
        "          a && b\n"
        "          cmd &\n"
        "          make 2>&1\n"
        "          echo $* \"$@\"\n"
    ),
    "glob in single-line run": "      - run: ls *.zip\n",
}


class Rule(unittest.TestCase):
    def test_must_fail(self):
        for name, text in MUST_FAIL.items():
            with self.subTest(name):
                self.assertTrue(lines(text), name)

    def test_must_pass(self):
        for name, text in MUST_PASS.items():
            with self.subTest(name):
                self.assertEqual(lines(text), [], name)

    def test_anchor_fails_file_once(self):
        found = list(cwe.findings("a: &x 1\nb: *x\n"))
        self.assertEqual(found, [(1, "anchors and aliases are not read")])

    def test_old_tag_step_fails_on_its_run_line(self):
        old = (
            "      - name: The tag names the package\n"
            "        if: startsWith(github.ref, 'refs/tags/pipos-')\n"
            '        run: test -f "build/${{ github.ref_name }}.zip"\n'
        )
        self.assertEqual(lines(old), [3])

    def test_repo_workflows_pass(self):
        folder = os.path.join(ROOT, ".github", "workflows")
        for name in sorted(os.listdir(folder)):
            if name.endswith((".yml", ".yaml")):
                with open(os.path.join(folder, name), encoding="utf-8") as handle:
                    self.assertEqual(lines(handle.read()), [], name)

    def test_yaml_file_is_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "w.yaml")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(MUST_FAIL["run"])
            run = subprocess.run(
                [sys.executable, "-I", TOOL, path], capture_output=True, text=True
            )
            self.assertEqual(run.returncode, 1)
            self.assertIn("w.yaml:2:", run.stdout)

    def test_usage(self):
        self.assertEqual(cwe.main([]), 2)
        self.assertEqual(cwe.main(["--no-pins"]), 2)


SHA = "11d5960a326750d5838078e36cf38b85af677262"

PIN_FAIL = {
    "major tag": "      - uses: actions/checkout@v4\n",
    "release tag": "        uses: actions/checkout@v4.4.0\n",
    "branch": "      - uses: actions/checkout@main\n",
    "short commit": "      - uses: actions/checkout@11d5960 # v4.4.0\n",
    "no comment": "      - uses: actions/checkout@%s\n" % SHA,
    "empty comment": "      - uses: actions/checkout@%s #\n" % SHA,
    "upper hex": "      - uses: actions/checkout@%s # v4\n" % SHA.upper(),
    "no ref": "      - uses: actions/checkout\n",
    "workflow at branch": (
        "    uses: RobCo-Industries/RobCo-Core/.github/workflows/robco-ci.yml@main\n"
    ),
    "docker tag": "      - uses: docker://alpine:3.20\n",
    "expression": "      - uses: ${{ x }}\n",
    "empty": "      - uses:\n",
}

PIN_PASS = {
    "action": "      - uses: actions/checkout@%s # v4.4.0\n" % SHA,
    "subfolder action": "        uses: github/codeql-action/init@%s # v3.1.0\n" % SHA,
    "workflow": (
        "    uses: RobCo-Industries/RobCo-Core/.github/workflows/robco-ci.yml@%s\n" % SHA
    ),
    "local action": "      - uses: ./.github/actions/setup\n",
    "local workflow": "    uses: ./.github/workflows/build.yml\n",
    "docker digest": "      - uses: docker://alpine@sha256:%s\n" % ("a" * 64),
    "word inside another word": "      - name: The check refuses a host\n",
    "word in a comment": "      # uses: actions/checkout@v4\n",
    "word in block text": "      - run: |\n          echo uses: x@v4\n",
}

TAG = "actions/checkout@v4"
PIN_FORM = {
    "space before colon": "      - uses : %s\n" % TAG,
    "double quoted key": '      - "uses": %s\n' % TAG,
    "single quoted key": "      - 'uses': %s\n" % TAG,
    "flow mapping": "      - { uses: %s }\n" % TAG,
    "flow sequence": "    steps: [{uses: %s}]\n" % TAG,
    "quoted value": "      - uses: '%s@%s' # v4\n" % (TAG[:-3], SHA),
    "double quoted value": '      - uses: "%s"\n' % TAG,
    "tab indent": "\t- uses: %s\n" % TAG,
    "tab separator": "      - uses:\t%s\n" % TAG,
    "two spaces": "      - uses:  %s\n" % TAG,
    "after other content": "      - name: x uses: %s\n" % TAG,
    "after a key": "      - name: uses: %s\n" % TAG,
    "upper case key": "      - Uses: %s\n" % TAG,
    "block scalar": "      - uses: |\n          %s\n" % TAG,
    "folded scalar": "      - uses: >-\n          %s\n" % TAG,
    "multi-line value": "    steps:\n      - uses: actions/checkout\n          @v4\n",
    "value on next line": "    steps:\n      - uses:\n          %s\n" % TAG,
    "continuation": "    steps:\n      - run: echo a\n          uses: %s\n" % TAG,
    "explicit key": "      - ? uses\n        : %s\n" % TAG,
    "tagged value": "      - uses: !!str %s\n" % TAG,
}

PIN_FORM_FILE = {
    "anchor": "x: &a 1\nsteps:\n  - uses: %s@%s # v4\n" % (TAG[:-3], SHA),
    "alias": "steps:\n  - uses: *a\n",
    "merge key": "steps:\n  - <<: {}\n    uses: %s@%s # v4\n" % (TAG[:-3], SHA),
    "two documents": "a: 1\n---\nsteps:\n  - uses: %s@%s # v4\n" % (TAG[:-3], SHA),
    "byte order mark": "\ufeffsteps:\n  - uses: %s@%s # v4\n" % (TAG[:-3], SHA),
    "carriage return": "steps:\r\n  - uses: %s@%s # v4\r\n" % (TAG[:-3], SHA),
}


class Pins(unittest.TestCase):
    def pins(self, text):
        return [num for num, _ in cwe.pin_findings(text)]

    def test_must_fail(self):
        for name, text in PIN_FAIL.items():
            with self.subTest(name):
                self.assertEqual(self.pins(text), [1], name)

    def test_must_pass(self):
        for name, text in PIN_PASS.items():
            with self.subTest(name):
                self.assertEqual(self.pins(text), [], name)

    def test_form_fails(self):
        for name, text in PIN_FORM.items():
            with self.subTest(name):
                found = list(cwe.pin_findings(text))
                self.assertTrue(found, name)
                self.assertEqual({r for _, r in found}, {cwe.FORM}, name)

    def test_form_fails_file(self):
        for name, text in PIN_FORM_FILE.items():
            with self.subTest(name):
                found = list(cwe.pin_findings(text))
                self.assertEqual(len(found), 1, name)
                self.assertTrue(found[0][1].startswith(cwe.FORM), name)

    def test_plain_form(self):
        good = "    steps:\n      - uses: %s@%s # v4\n" % (TAG[:-3], SHA)
        self.assertEqual(list(cwe.pin_findings(good)), [])
        bad = "    steps:\n      - uses: %s\n" % TAG
        self.assertEqual(
            list(cwe.pin_findings(bad)), [(2, "uses: not pinned by a full commit")]
        )
        first = "---\n" + good
        self.assertEqual(list(cwe.pin_findings(first)), [])

    def test_cli_names_the_plain_form(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "w.yml")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(PIN_FORM["space before colon"])
            run = subprocess.run(
                [sys.executable, "-I", TOOL, path], capture_output=True, text=True
            )
            self.assertEqual(run.returncode, 1)
            self.assertIn("cannot check; write uses: OWNER/REPO@<commit> # <tag>", run.stdout)

    def test_cli_runs_both_rules_and_no_pins_skips_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "w.yml")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(PIN_FAIL["major tag"])
            run = subprocess.run(
                [sys.executable, "-I", TOOL, path], capture_output=True, text=True
            )
            self.assertEqual(run.returncode, 1)
            self.assertIn("w.yml:1: uses: not pinned by a full commit", run.stdout)
            run = subprocess.run(
                [sys.executable, "-I", TOOL, "--no-pins", path],
                capture_output=True,
                text=True,
            )
            self.assertEqual((run.returncode, run.stdout), (0, ""))

    def test_repo_workflows_are_pinned(self):
        folder = os.path.join(ROOT, ".github", "workflows")
        for name in sorted(os.listdir(folder)):
            if name.endswith((".yml", ".yaml")):
                with open(os.path.join(folder, name), encoding="utf-8") as handle:
                    self.assertEqual(self.pins(handle.read()), [], name)


if __name__ == "__main__":
    unittest.main()
