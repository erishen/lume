#!/usr/bin/env python3
"""Gate the builtin-name diff between lume-core and the lume host.

The two registries are deliberately different (see tests/builtin-parity.allow),
but an *unreviewed* change to that difference is not: a new core builtin that
the host silently never adopted, or a host builtin core never heard of, means
a copied test fixture can fail with "undefined variable" and the two trees can
drift apart without anybody noticing. `make check-parity` compares the live
registries against the explicit accept list and fails on any difference.

With no sibling lume-core tree the check is skipped (same fallback discipline
as sync-lang.py falling back to PIN.manifest). CI's check-sync job and the
release verify job both check lume-core out next door, so there it is a real
check, not a skip. Override the location with LUME_CORE_DIR.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.environ.get("LUME_CORE_DIR") or os.path.join(ROOT, "..", "lume-core")
CORE_INTERP = os.path.join(CORE, "src", "interp.c")
HOST_INTERP = os.path.join(ROOT, "lang", "interp.c")
ALLOW = os.path.join(ROOT, "tests", "builtin-parity.allow")

# `{"name", b_name}` rows in the interpreter's builtin registry.
PAT = re.compile(r'\{\s*"([a-z_][a-z0-9_]*)"\s*,\s*b_\w+\s*\}')

RED = "\033[31m"
GREEN = "\033[32m"
OFF = "\033[0m"


def names(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return set(PAT.findall(fh.read()))


def accept_list():
    """{section: set(names)} from tests/builtin-parity.allow."""
    out = {"core-only": set(), "host-only": set()}
    section = None
    with open(ALLOW, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
            if section in out:
                out[section].add(line)
    return out


def main():
    if not os.path.isfile(CORE_INTERP):
        print("skip  builtin parity: no lume-core at %s" % CORE_INTERP)
        print("      (CI checks out lume-core as a sibling: LUME_CORE_DIR=... to override)")
        return 0
    if not os.path.isfile(HOST_INTERP):
        print("%sFAIL%s missing %s" % (RED, OFF, HOST_INTERP))
        return 1
    if not os.path.isfile(ALLOW):
        print("%sFAIL%s missing %s" % (RED, OFF, ALLOW))
        return 1

    core, host = names(CORE_INTERP), names(HOST_INTERP)
    actual = {"core-only": core - host, "host-only": host - core}
    accepted = accept_list()

    bad = []
    for section in ("core-only", "host-only"):
        new = sorted(actual[section] - accepted[section])
        stale = sorted(accepted[section] - actual[section])
        for n in new:
            bad.append("new %-9s %s (not accepted)" % (section, n))
        for n in stale:
            bad.append("stale %-9s %s (no longer in the diff)" % (section, n))

    if bad:
        for b in bad:
            print("%sFAIL%s %s" % (RED, OFF, b))
        print("builtin parity changed. Either port the builtin to the other tree,")
        print("or update tests/builtin-parity.allow with the explicit decision.")
        return 1

    print("%sok%s   builtin parity: %d core-only / %d host-only accepted, "
          "%d shared builtins (%d core, %d host)"
          % (GREEN, OFF, len(actual["core-only"]), len(actual["host-only"]),
             len(core & host), len(core), len(host)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
