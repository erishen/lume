#!/usr/bin/env python3
"""Keep lume's lang/ tree pinned to a lume-core commit.

lang/ is the host's copy of the language front end (lexer -> parser ->
typecheck -> value -> interp -> bridge).  It must be traceable back to a
specific lume-core commit so `bin/lume` can always answer "which language
am I built from".  See lang/PIN.

    python3 scripts/sync-lang.py check   # report drift, write nothing
    python3 scripts/sync-lang.py sync    # copy core's files over, refresh PIN

The core tree lives next to this repo by default; override with
LUME_CORE_DIR=<path> if it sits elsewhere.  Paths are never written into
PIN (this repo is public).

check reports three things:

  host-owned  files the host deliberately keeps; they are never overwritten
              and *not* counted as drift.  Each one also gets an upstream
              line delta - that is the point of the report: a pinned file
              that stops tracking upstream is invisible otherwise, and
              interp.c already proved it can be 489 lines behind.
  host-only   files that exist here and never existed upstream.
  drift       upstream has it, this copy differs.  The only bucket that
              makes check fail.

The upstream comparison needs a lume-core tree next door.  Where there is
none (CI, a checkout on its own) check falls back to the baseline in
lang/PIN.manifest: one md5 per synced file, written by `make sync-lang`.
That is what makes the pin verifiable without the upstream repo, and it is
the signal CI can act on today - erishen/lume-core exists but is empty,
so `git clone` cannot be made to resolve the pinned sha yet.
"""
import hashlib
import difflib
import io
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG = os.path.join(ROOT, "lang")
PIN = os.path.join(LANG, "PIN")
MANIFEST = os.path.join(LANG, "PIN.manifest")
DEFAULT_CORE = os.path.join(os.path.dirname(ROOT), "lume-core")

# Files the host owns and must never be replaced by the upstream copy.
# Every one of them exists because lume and lume-core genuinely diverged:
#
# lume.h  pulls in agenthttpd.h (tools/skills/sqlite_tool/minijson) - the
#         host-only integration lume-core dropped for a local sbuf.h.
# main.c  is the CLI entry point; the core version includes backend.h /
#         backend_llvm.h / codegen.h, which belong to the native backends
#         that lume has no business linking (agent-httpd owns it instead).
#         Copying it over breaks the build with "'backend.h' file not found".
# builtins_internal.h  defines ToolDef with its own TOOL_MAX; agent-httpd's
#         tools.h defines the same tag with 96.  lume-core never links
#         agent-httpd so it never saw the clash - here it is an immediate
#         redefinition error.
# builtins.c  routes the HTTP builtins through native_http_get(), which
#         lume-core defines in builtins_http.c - a file with no business in
#         this tree.  builtins.c, builtins_catalog.c and builtins_fs.c all
#         pull that header in and reach for ToolDef.path / SKILL_MAX, so
#         they ride with it.
# vdom.c  needs agent-httpd's sbuf.h (minijson.h only over there).
# interp.c's registry seeds b_http_get; lume-core implements it in
#         builtins_http.c, which drags OpenSSL into a host link that never
#         asked for it.  The host seeds its own registry instead.  Since
#         upstream grew http_get here is the host losing a builtin it never
#         had, and check reports it as a lag.
# builtins.h declares the seeded sql_query / sql_write on top of core's.
# typecheck.c / typecheck_expr.c / typecheck_stmt.c resolve names against
#         the seeded registry; the upstream checker rejects sql_*.
HOST_PINNED = {"lume.h", "main.c", "builtins_internal.h", "builtins.c",
               "builtins_catalog.c", "builtins_fs.c", "vdom.c",
               "interp.c", "builtins.h",
               "typecheck.c", "typecheck_expr.c", "typecheck_stmt.c"}

# Upstream lines behind in a pinned file stop being news at about this many;
# below it the file is just locally edited.
LAG_WARN = 40

GREEN = "\033[32m"
YELLOW = "\033[33m"
RED = "\033[31m"
OFF = "\033[0m"


def core_dir():
    d = os.environ.get("LUME_CORE_DIR") or DEFAULT_CORE
    if not os.path.isfile(os.path.join(d, "src", "interp.c")):
        sys.exit("lume-core tree not found at %s (LUME_CORE_DIR=... to override)" % d)
    return d


def core_sha(d):
    p = subprocess.run(["git", "-C", d, "rev-parse", "HEAD"],
                       capture_output=True, text=True)
    return p.stdout.strip() or "(unknown)"


def core_version(d):
    """What upstream calls this commit, if it has a tag at all.

    Falls back to the bare sha; the host's PIN should not claim a version
    when upstream has none.
    """
    p = subprocess.run(["git", "-C", d, "describe", "--tags", "--always"],
                       capture_output=True, text=True)
    return p.stdout.strip() or "(unknown)"


def md5(path):
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def lang_sources():
    """Every tracked language source in lang/, minus the PIN file.

    Names are bare: lang/ and the core's src/ use the same filenames, so the
    basename is what both sides agree on.
    """
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "lang"],
                         capture_output=True, text=True).stdout.split()
    return sorted(os.path.basename(f) for f in out
                  if f.endswith((".c", ".h")) and os.path.basename(f) != "PIN")


def synced_files(files):
    """The files sync actually copies, i.e. everything but host_owned."""
    return [f for f in files if f not in HOST_PINNED]


def read_manifest():
    """{filename: md5} from the committed baseline, or None if there is none."""
    if not os.path.isfile(MANIFEST):
        return None
    out = {}
    with open(MANIFEST, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, digest = line.partition(" ")
            if name:
                out[name] = digest
    return out


def read_pin_sha():
    """The sha lang/PIN claims to be pinned at, or "" if it has none."""
    if not os.path.isfile(PIN):
        return ""
    with open(PIN, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition(":")
            if key.strip() == "sha":
                return value.strip()
    return ""


def pin_status(core, sha):
    """What lang/PIN's sha means right now, next to the core tree.

    The pin records the core commit lang/ was copied from, so it only has
    to stop meaning something once that commit is no longer reachable from
    what the tree is built from.  The naive test - pin != core HEAD - is
    wrong in the direction that matters: a commit after the pin that
    touched no synced file (a `backups:` commit carrying a binary, a
    CHANGELOG edit) leaves lang/ exactly as current as it ever was, yet
    the tree reads as broken until somebody runs a `sync` that copies
    nothing.  Older-than-HEAD and newer-than-HEAD are different failures
    and get different words.

    Returns (state, note, suffix) - the suffix is what the closing "ok"
    line appends.  Only the failure states carry one.
    """
    pin = read_pin_sha()
    if not pin:
        return "none", "lang/PIN records no sha", " (lang/PIN has no SHA)"
    p = subprocess.run(["git", "-C", core, "rev-parse", "--verify", "--quiet",
                        pin + "^{commit}"], capture_output=True, text=True)
    if p.returncode != 0 or not p.stdout.strip():
        return ("missing", "lang/PIN records %s, which %s does not have"
                % (pin[:12], os.path.basename(core)), " (lang/PIN SHA STALE)")
    resolved = p.stdout.strip()
    if resolved == sha:
        return "current", "", ""
    after = subprocess.run(["git", "-C", core, "merge-base", "--is-ancestor",
                            resolved, sha])
    if after.returncode == 0:
        n = subprocess.run(["git", "-C", core, "rev-list", "--count",
                            "%s..%s" % (resolved, sha)],
                           capture_output=True, text=True)
        note = ("lang/PIN is at %s, %s has %s more commit(s) since - none of "
                "them touched a synced file, so lang/ is still exactly "
                "upstream's" % (pin[:12], os.path.basename(core),
                                n.stdout.strip()))
        return "folded", note, ""
    before = subprocess.run(["git", "-C", core, "merge-base", "--is-ancestor",
                             sha, resolved])
    if before.returncode == 0:
        return ("ahead", "lang/PIN is at %s but %s has moved back to %s - "
                "lang/ is newer than the tree it came from"
                % (pin[:12], os.path.basename(core), sha[:12]),
                " (lang/PIN SHA STALE)")
    return ("diverged", "lang/PIN is at %s, unrelated to %s at %s"
            % (pin[:12], os.path.basename(core), sha[:12]),
            " (lang/PIN SHA STALE)")


def upstream_files(files, core):
    """What `sync` would actually copy: synced files upstream has.

    Host-only files (bridge, iquest) live here and never upstream, so they
    are deliberately left out of the baseline - the host is allowed to edit
    them by hand without a pin bump.
    """
    if core is None:
        return None
    return [f for f in synced_files(files)
            if os.path.isfile(os.path.join(core, "src", f))]


def write_manifest(files, core):
    """Record the current md5 of every upstream file as the new baseline."""
    with open(MANIFEST, "w", encoding="utf-8") as fh:
        fh.write("# md5 of every synced lang/ file, written by `make sync-lang`.\n")
        fh.write("# check compares lang/ against this when lume-core is not reachable.\n")
        for f in upstream_files(files, core) or []:
            p = os.path.join(LANG, f)
            if os.path.isfile(p):
                fh.write("%s %s\n" % (f, md5(p)))


def check_manifest(files, core):
    """Problems the committed baseline reveals, or None when it is absent.

    A synced file that differs from its baseline is exactly what a hand
    edit looks like: it lands in git without a pin bump, so without this
    the tree drifts silently until somebody runs check-sync on a machine
    that has lume-core checked out next door.  With the core tree present
    the tracked set is what upstream has; without it the baseline's own
    lines are the set, so CI still catches edits when lume-core is not
    published.
    """
    baseline = read_manifest()
    if baseline is None:
        return None
    tracked = upstream_files(files, core)
    if tracked is None:
        tracked = sorted(baseline)
    bad = []
    for f in tracked:
        if f not in baseline:
            bad.append("%sNEW%s      %-18s no baseline, run `make sync-lang`" % (RED, OFF, f))
        elif not os.path.isfile(os.path.join(LANG, f)):
            bad.append("%sDRIFT%s     %-18s baseline entry, file is gone" % (RED, OFF, f))
        elif baseline[f] != md5(os.path.join(LANG, f)):
            bad.append("%sDRIFT%s     %-18s differs from lang/PIN.manifest" % (RED, OFF, f))
    for f in sorted(baseline):
        if f not in tracked:
            bad.append("%sSTALE%s     %-18s baseline entry, not a synced file" % (YELLOW, OFF, f))
    return bad


def upstream_delta(name):
    """(lines upstream has that we do, lines we have that upstream does).

    None when upstream has no such file.  Only used for host_owned files:
    the synced ones are already byte-identical upstream, and the host-only
    ones are the host's alone.
    """
    cs, ls = os.path.join(core_dir(), "src", name), os.path.join(LANG, name)
    if not (os.path.isfile(cs) and os.path.isfile(ls)):
        return None
    try:
        a = io.open(cs, encoding="utf-8", errors="replace").read().splitlines()
        b = io.open(ls, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return None
    # unified_diff(a=upstream, b=ours): '-' lines exist upstream only.
    diff = difflib.unified_diff(a, b, lineterm="")
    up = sum(1 for x in diff if x.startswith("-") and not x.startswith("---"))
    ours = sum(1 for x in diff if x.startswith("+") and not x.startswith("+++"))
    return up, ours


LAG = os.path.join(LANG, "PIN.lag")


def read_lag():
    """{filename: accepted upstream line count} from lang/PIN.lag.

    A host-owned file is behind upstream because the two trees genuinely
    fork: lume links agent-httpd and seeds its own registry, lume-core
    links neither.  Counting those lines every time is a report that is
    always wrong in the same direction, so what is accepted gets written
    down once with `sync-lang.py accept-lag` and only *growth* past that
    number reads as news.  Without it check ends every run with "a
    hand-merge is owed" and the line is worth nothing.
    """
    out = {}
    if not os.path.isfile(LAG):
        return out
    with open(LAG, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, n = line.partition(" ")
            if name:
                try:
                    out[name] = int(n)
                except ValueError:
                    pass
    return out


def write_lag():
    """Record today's upstream line count for every host-owned file."""
    with open(LAG, "w", encoding="utf-8") as fh:
        fh.write("# upstream line count accepted for each host-owned file,\n")
        fh.write("# written by `sync-lang.py accept-lag`.  check reports growth\n")
        fh.write("# past this number, not the number itself.\n")
        for f in sorted(HOST_PINNED):
            d = upstream_delta(f)
            if d:
                fh.write("%s %d\n" % (f, d[0]))


def write_pin(sha, version):
    body = (
        "# lang/ is a pinned copy of the lume-core language tree.\n"
        "# Bump it with:  make sync-lang && make check-sync\n"
        "# Detect drift without touching anything:  make check-sync\n"
        "#\n"
        "# Everything under host_owned is never overwritten: each one exists\n"
        "# because lume and lume-core genuinely diverged (agenthttpd\n"
        "# integration, the native-backend CLI, the ToolDef tag, the seeded\n"
        "# sql_* builtins).  A bump may still need a hand-merge in them, and\n"
        "# check-sync reports how far behind upstream each one is.\n"
        "# The core-specific files (codegen*/backend*/llvm_codegen*/rt.c) are\n"
        "# not copied at all - lume has no native backend, agent-httpd owns it.\n"
        "core_dir: ../lume-core\n"
        "sha: %s\n"
        "version: %s\n"
        "synced_at: %s\n"
        "host_owned: %s\n"
    ) % (sha, version,
         subprocess.run(["date", "+%Y-%m-%d %H:%M:%S"],
                        capture_output=True, text=True).stdout.strip(),
         " ".join(sorted(HOST_PINNED)) or "-")
    with open(PIN, "w", encoding="utf-8") as fh:
        fh.write(body)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    if mode not in ("sync", "check", "accept-lag"):
        sys.exit("usage: sync-lang.py {sync|check|accept-lag}")

    files = lang_sources()

    if mode == "check":
        # The upstream comparison needs lume-core next door.  CI has no such
        # tree, and one that exists but carries no commits (erishen/lume-core
        # is an empty repo today) is the same thing as far as this script is
        # concerned, so treat both as "core unavailable" and lean on the
        # committed baseline instead.
        core = None
        try:
            core = core_dir()
        except SystemExit:
            pass
        if core:
            sha = core_sha(core)
        else:
            sha = "(no lume-core tree)"
            print("==> no lume-core tree here; content drift is not checked - "
                  "comparing lang/ against the lang/PIN.manifest baseline only")

        # baseline first: the one leg that must always run
        bad = check_manifest(files, core)
        if bad is None:
            print("%sok%s lang/PIN.manifest does not exist yet; "
                  "run `make sync-lang` to record it" % (GREEN, OFF))
        else:
            for line in bad:
                print(line)
            if bad:
                print("%d lang/ file(s) differ from lang/PIN.manifest" % len(bad))
                return 1
        if not core:
            # Say so rather than ending on silence: the job is green because
            # the baseline holds, not because nothing was compared.
            n = len(read_manifest() or {})
            print("%sok%s lang/ still matches its committed baseline - "
                  "%d synced file(s) in lang/PIN.manifest; upstream content "
                  "was not compared without lume-core" % (GREEN, OFF, n))
            return 0

        # three buckets, only one of which is an actual problem:
        #   host-owned  -> deliberately pinned, needs a human on upgrade
        #   host-only   -> exists here, never existed upstream (bridge, iquest)
        #   drift       -> upstream has it, this copy differs: the real signal
        host_only, drift = [], []
        for f in files:
            if f in HOST_PINNED:
                continue
            cs, ls = os.path.join(core, "src", f), os.path.join(LANG, f)
            if not os.path.isfile(cs):
                if os.path.isfile(ls):
                    host_only.append(f)
                continue
            if md5(cs) != md5(ls):
                drift.append(f)
        for f in host_only:
            print("host-only   %s" % f)

        # Host-owned files are skipped above, so their lag would never show
        # up anywhere.  Report it instead of failing: a pinned file that is
        # behind upstream is a decision, not a broken tree.
        lagging = []
        accepted = read_lag()
        for f in sorted(HOST_PINNED):
            d = upstream_delta(f)
            if not d or d[0] == 0:
                continue
            up, ours = d
            note = "upstream +%d" % up
            if ours:
                note += " / local +%d" % ours
            base = accepted.get(f)
            if base is None:
                # Nothing accepted yet: fall back to the raw count, and say
                # the baseline is what would make this line mean something.
                if up >= LAG_WARN:
                    lagging.append(f)
                    print("%sBEHIND%s     %-18s %s - no lang/PIN.lag yet, "
                          "run `sync-lang.py accept-lag`" % (YELLOW, OFF, f, note))
                else:
                    print("host-owned  %-18s %s" % (f, note))
                continue
            grew = up - base
            if grew > 0:
                lagging.append(f)
                print("%sBEHIND%s     %-18s %s, +%d since the last acceptance"
                      % (YELLOW, OFF, f, note, grew))
            else:
                print("host-owned  %-18s %s (accepted)" % (f, note))

        for f in drift:
            print("%sDRIFT%s   %s" % (RED, OFF, f))
        if drift:
            print("%d of %d synced file(s) differ from core; run `make sync-lang`"
                  % (len(drift), len(files) - len(HOST_PINNED)))
            return 1

        # The pin is documentation: nothing in `check` ever compared lang/PIN's
        # own sha field against the core tree, so a pin left behind by a
        # `sync` that never ran still read as green. Say so on the same line
        # that reports green - a warning nobody reads is the same as the
        # silence this used to have.  Compare by ancestry, not equality: a
        # later commit that left the synced files alone does not make the
        # pin stale.
        state, pin_note, suffix = pin_status(core, sha)
        if state in ("ahead", "missing", "diverged", "none"):
            print("%sSTALEPIN%s %s - run `make sync-lang`"
                  % (YELLOW, OFF, pin_note))
        elif pin_note:
            # Green, but say why the pin is not HEAD anyway.
            print("pin         %s" % pin_note)
        print("%sok%s lang/ matches %s @ %s - %d synced, %d host-owned, %d host-only%s"
              % (GREEN, OFF, os.path.basename(core), sha[:12],
                 len(files) - len(HOST_PINNED), len(HOST_PINNED), len(host_only),
                 suffix))
        if lagging:
            print("%d host-owned file(s) are %d+ lines behind upstream; "
                  "a hand-merge is owed" % (len(lagging), LAG_WARN))
        return 0

    if mode == "accept-lag":
        core_dir()          # upstream_delta reads through the core tree
        write_lag()
        print("%sok%s recorded the current upstream line count for %d "
              "host-owned file(s) in lang/PIN.lag; check now reports only "
              "growth past it" % (GREEN, OFF, len(HOST_PINNED)))
        return 0

    core = core_dir()
    sha = core_sha(core)
    changed = []
    for f in files:
        src = os.path.join(core, "src", f)
        dst = os.path.join(LANG, f)
        if f in HOST_PINNED or not os.path.isfile(src):
            continue
        if not os.path.isfile(dst):
            continue
        if md5(src) == md5(dst):
            continue
        with open(src, "rb") as fh:
            data = fh.read()
        with open(dst, "wb") as fh:
            fh.write(data)
        changed.append(f)
    write_pin(sha, core_version(core))
    write_manifest(files, core)   # new baseline for anyone else running check
    if changed:
        print("%sok%s synced %d file(s) from %s @ %s" % (GREEN, OFF, len(changed),
                                                         os.path.basename(core), sha[:12]))
        for f in changed:
            print("     ~ %s" % f)
    else:
        print("%sok%s lang/ already matches %s @ %s" % (GREEN, OFF,
                                                        os.path.basename(core), sha[:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
