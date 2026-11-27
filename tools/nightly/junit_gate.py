#!/usr/bin/env python3
"""Judge one nightly test job from its pytest JUnit file.

    junit_gate.py --job NAME FIRST.xml [RERUN.xml] --summary F

The job is red, exit 1, when the file is missing or unreadable, when it
holds no test, when a test failed or errored, when no test passed, or on
a skip. One skip per job whose reason starts with "host timing:" passes,
the suite's rule for a guest starved by a busy host. A night where every
whole-chain test skips proved nothing, so it is red.

It appends a verdict and one line per test to --summary, and prints an
error annotation per red reason.

The rerun file comes from `pytest --lf` after a failed test step. A
retry never turns a red job green. It only says which kind of red:

- A test that failed or errored in the first file is FLAKY when it
  passed in the rerun, and BROKEN when it did not pass there or is
  missing from it. Each gets one annotation with its label.
- A rerun file with no failure in the first file is red: a rerun only
  follows a failure, so the two files do not belong together.
- A rerun file that is missing or unreadable is red. The failed tests
  of the first file stay red without a label.
- A rerun that ran a test which did not fail in the first file is red,
  one reason per such test. The failed tests still get their labels.

Standard library only.
"""
import argparse
import sys
import xml.etree.ElementTree as ET

HOST_TIMING = "host timing:"


def read_cases(path):
    """[(name, outcome, message)] from a JUnit file. Raises ValueError."""
    try:
        root = ET.parse(path).getroot()
    except FileNotFoundError:
        raise ValueError(f"{path} is missing") from None
    except OSError as e:
        raise ValueError(f"cannot read {path}: {e.strerror}") from None
    except ET.ParseError as e:
        raise ValueError(f"{path} is not valid XML: {e}") from None
    if root.tag not in ("testsuites", "testsuite"):
        raise ValueError(f"{path} is not a JUnit file: root is <{root.tag}>")
    cases = []
    for case in root.iter("testcase"):
        name = f"{case.get('classname', '')}::{case.get('name', '')}".lstrip(":")
        outcome, message = "passed", ""
        for tag, label in (("error", "errored"), ("failure", "failed"),
                           ("skipped", "skipped")):
            node = case.find(tag)
            if node is not None:
                outcome = label
                message = " ".join((node.get("message") or node.text or "").split())
                break
        cases.append((name, outcome, message))
    return cases


def judge(cases):
    """List of red reasons, empty when the job is green."""
    reasons = []
    if not cases:
        return ["the file holds no test"]
    for name, outcome, message in cases:
        if outcome in ("failed", "errored"):
            reasons.append(f"{name} {outcome}")
    skips = [(n, m) for n, o, m in cases if o == "skipped"]
    timing = [s for s in skips if s[1].startswith(HOST_TIMING)]
    for name, message in skips:
        if (name, message) not in timing[:1]:
            reasons.append(f"{name} skipped: {message}")
    if not any(o == "passed" for _, o, _ in cases):
        reasons.append("no test passed")
    return reasons


def label(cases, rerun_path):
    """Red reasons from the rerun file, and {name: label} for failed tests."""
    failed = [n for n, o, _ in cases if o in ("failed", "errored")]
    if not failed:
        return [f"{rerun_path} is a rerun file, but no test failed in the first file"], {}
    try:
        rerun = read_cases(rerun_path)
    except ValueError as e:
        return [f"rerun: {e}"], {}
    outcomes = {}
    for name, outcome, _ in rerun:
        outcomes.setdefault(name, []).append(outcome)
    labels = {n: "FLAKY" if outcomes.get(n) == ["passed"] else "BROKEN" for n in failed}
    reasons = [f"{n} ran in the rerun but did not fail in the first file"
               for n in outcomes if n not in labels]
    return reasons, labels


def run(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--job", required=True)
    p.add_argument("junit", nargs="+")
    p.add_argument("--summary", required=True)
    args = p.parse_args(argv)
    if len(args.junit) > 2:
        p.error("one JUnit file and at most one rerun file")
    labels = {}
    try:
        cases = read_cases(args.junit[0])
        reasons = judge(cases)
    except ValueError as e:
        cases, reasons = [], [str(e)]
    if len(args.junit) == 2 and cases:
        extra, labels = label(cases, args.junit[1])
        reasons += extra
    elif len(args.junit) == 2:
        reasons.append("the rerun file is not judged without a first file")
    for i, r in enumerate(reasons):
        for name, lab in labels.items():
            if r in (f"{name} failed", f"{name} errored"):
                reasons[i] = f"{lab}: {r}" + (", passed on the rerun" if lab == "FLAKY"
                                              else ", did not pass on the rerun")
    counts = {o: sum(1 for _, oc, _ in cases if oc == o)
              for o in ("passed", "failed", "errored", "skipped")}
    verdict = "red" if reasons else "green"
    lines = [f"### {args.job}: {verdict}", "",
             ", ".join(f"{n} {o}" for o, n in counts.items()), ""]
    lines += [f"- Red: {r}" for r in reasons]
    lines += [f"- `{n}`: {o}" + (f", {m}" if m and o != "passed" else "")
              + (f", {labels[n]}" if n in labels else "")
              for n, o, m in cases]
    text = "\n".join(lines) + "\n\n"
    with open(args.summary, "a", encoding="utf-8") as f:
        f.write(text)
    print(text, end="")
    for r in reasons:
        print(f"::error title={args.job}::{r}")
    return 1 if reasons else 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
