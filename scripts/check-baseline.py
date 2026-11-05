#!/usr/bin/env python3
"""Re-generate the cross-repo semantic baseline with lume-core's interpreter.

tests/baseline-*.expected are the authoritative outputs of bin/lume-core on
two pure-language fixtures.  tests/run_all.sh diffs bin/lume against them,
and CI cmp's the .lume copies against the sibling core checkout - but
nothing ever regenerated the .expected files.  Editing an .expected by hand
(or a core semantics change that leaves the fixture text untouched) could
therefore make a wrong host interpreter look green.

CI's check-sync job and release.yml's verify job build the sibling core
tree first, then run this script: the expected output is produced on the
spot and compared byte-for-byte.

    python3 scripts/check-baseline.py                 # ../lume-core/bin/lume-core
    LUME_CORE_DIR=/path python3 scripts/check-baseline.py
    LUME_CORE_BIN=/path/to/lume-core python3 scripts/check-baseline.py
"""

import difflib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.environ.get("LUME_CORE_DIR") or os.path.join(
    os.path.dirname(ROOT), "lume-core")
TESTS = os.path.join(ROOT, "tests")

# (host baseline basename, upstream core fixture)
PAIRS = [
    ("baseline-literals-bitops", "literals-bitops-consistency.lume"),
    ("baseline-block-scope", "p1-9-consistency.lume"),
]


def core_binary():
    override = os.environ.get("LUME_CORE_BIN")
    if override:
        return override if os.path.isfile(override) else None
    for name in ("lume-core", "lume-core.exe"):
        path = os.path.join(CORE, "bin", name)
        if os.path.isfile(path):
            return path
    return None


def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read().replace(b"\r\n", b"\n")


def first_diff(got, want, label):
    a = want.decode("utf-8", "replace").splitlines()
    b = got.decode("utf-8", "replace").splitlines()
    lines = list(difflib.unified_diff(a, b, fromfile=label + ".expected",
                                      tofile="lume-core output", lineterm=""))
    for line in lines[:20]:
        print("      " + line)


def main():
    binary = core_binary()
    if not binary:
        print("FAIL  no lume-core interpreter at %s/bin/lume-core" % CORE)
        print("      build it with `make -C %s`, or set LUME_CORE_BIN" % CORE)
        return 1
    print("      core interpreter: %s" % binary)

    bad = []
    for base, upstream in PAIRS:
        core_fixture = os.path.join(CORE, "tests", upstream)
        host_fixture = os.path.join(TESTS, base + ".lume")
        expected = os.path.join(TESTS, base + ".expected")
        for path in (core_fixture, host_fixture, expected):
            if not os.path.isfile(path):
                print("FAIL  %s missing %s" % (base, path))
                bad.append(base)
                break
        else:
            if read_bytes(core_fixture) != read_bytes(host_fixture):
                print("FAIL  %s.lume drifted from lume-core's %s"
                      % (base, upstream))
                bad.append(base)
                continue
            proc = subprocess.run([binary, host_fixture], cwd=ROOT,
                                  capture_output=True)
            if proc.returncode != 0:
                print("FAIL  %s: lume-core exited %d: %s"
                      % (base, proc.returncode,
                         proc.stderr.decode("utf-8", "replace").strip()[:200]))
                bad.append(base)
                continue
            got = proc.stdout.replace(b"\r\n", b"\n")
            want = read_bytes(expected)
            if got != want:
                print("FAIL  %s.expected does not match lume-core output "
                      "(%d bytes vs %d)" % (base, len(want), len(got)))
                first_diff(got, want, base)
                bad.append(base)
                continue
            print("ok    %-26s %d bytes, matches lume-core"
                  % (base, len(got)))

    if bad:
        print("baseline mismatch: re-generate the .expected files with the "
              "core interpreter, then verify bin/lume still matches")
        return 1
    print("ok    semantic baseline regenerated from lume-core (%d fixtures)"
          % len(PAIRS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
