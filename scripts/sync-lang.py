#!/usr/bin/env python3
"""Keep lume's lang/ tree pinned to a lume-core commit.

lang/ is the host's copy of the language front end (lexer -> parser ->
typecheck -> value -> interp -> bridge).  It must be traceable back to a
specific lume-core commit so `bin/lume` can always answer "which language
am I built from".  See lang/PIN.

    python3 scripts/sync-lang.py check   # report drift, write nothing
    python3 scripts/sync-lang.py check --strict   # fail on stale pin / new lag
    python3 scripts/sync-lang.py sync    # copy core's files over, refresh PIN

The core tree is located through, in order: LUME_CORE_DIR, the core_dir
line in lang/PIN (relative to this repo), then the ../lume-core sibling.
Paths are never written into PIN (this repo is public).

check reports five things:

  host-owned  files the host deliberately keeps; they are never overwritten
              and *not* counted as drift.  Each one also gets an upstream
              line delta - that is the point of the report: a pinned file
              that stops tracking upstream is invisible otherwise, and
              interp.c already proved it can be 489 lines behind.
  host-only   files that exist here and never existed upstream.
  drift       upstream has it, this copy differs.  The only bucket that
              makes check fail.
  registry    a builtin seeded in interp.c that LUME_BUILTIN_NAMES[] cannot
              name - a partial hand-merge, always a failure.
  unclassified  an upstream src/*.c|h file that is neither in lang/ nor
              explicitly named as core-only.  lang_sources() is derived
              from `git ls-files lang`, so without this leg a language
              source added upstream would be invisible to both sync and
              check: never copied, never baselined, never reported.

`check --strict` also fails on a stale PIN (ahead/missing/diverged/none),
on any host-owned file whose upstream delta grew past lang/PIN.lag, and on
an upstream edit that changed a host-owned file's content *without* growing
that line count (an upstream deletion reads as `+` lines on our side, so
the count alone cannot see it).  A PIN that is merely folded (core moved on
without touching a synced file) stays green.

The upstream comparison needs a lume-core tree next door.  Where there is
none, check falls back to lang/PIN.manifest (one md5 per synced file,
written by `make sync-lang`) and returns green.  LUME_REQUIRE_CORE=1 turns
that fallback into a hard failure: CI sets it, so a broken sibling checkout
can never quietly turn the gate into a no-op.
"""
import hashlib
import datetime
import difflib
import io
import os
import re
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
# builtins_http.c is synced upstream like the rest, but it also carries
#         HTTP_DBG, the three debug points that compile to nothing unless
#         -DLUME_HTTP_DEBUG is passed.  Upstream prints them
#         unconditionally, which is fine for a CLI and not for lume, where
#         http_get backs a long-running server and one failed call used to
#         be one permanent line of stderr.  Pinned so `sync` cannot quietly
#         make the host log noisy again.
HOST_PINNED = {"lume.h", "main.c", "builtins_internal.h", "builtins.c",
               "builtins_catalog.c", "builtins_fs.c", "vdom.c",
               "interp.c", "builtins.h", "builtins_http.c",
               "typecheck.c", "typecheck_expr.c", "typecheck_stmt.c",
               # Host builtins_math.c is a self-contained single-unit file (basic
               # numerics + distribution CDFs + the #54 technical-indicator
               # sequences: rolling_max/min, kdj, williams_r, atr, obv, ported
               # interpreter-only). Upstream's builtins_math.c is a 27-line hub
               # that #includes three .inc fragments the host tree does not carry
               # (host is one translation unit, no .inc splitting). Syncing the
               # hub over would #include files that don't exist and break the
               # host build, so pin it and track the drift with the lag report.
               "builtins_math.c",
               # Windows (MSYS2/mingw) build replaces bridge.c with this stub;
               # lume's copy adds a bridge_run() that errors out because this
               # build links no HTTP server. Pinned so `sync` cannot silently
               # swap in lume-core's bridge_stub.c (which defers to
               # bridge_serve.c) and break the Windows link.
               "bridge_stub.c",
               # Upstream grew its own builtins_sql.c (ce6ed80, self-contained
               # language-level SQLite) AFTER this host had carried its own
               # lang/builtins_sql.c for months - one with app-specific table
               # policy (the portfolio-mirror rule) that upstream explicitly
               # called host business and left out. The two files share a name
               # and builtin names but not ownership; keep ours, let the lag
               # report track upstream's.
               "builtins_sql.c",
               # value.c: upstream's copy grew the OBJ_SQLTX transaction-handle
               # object (P6 three-backend sql tx, builtins_sql.c is the only TU
               # that knows sqlite3) which this host has never adopted - its sql
               # surface is the app-specific port (see builtins_sql.c above)
               # with sql_query/sql_write only and no tx objects, and lume.h
               # here carries no OBJ_SQLTX kind. `sync` would overwrite
               # value.c wholesale and break the build, so pin it and track the
               # gap as BEHIND; the GC/mark logic beyond the OBJ_SQLTX block
               # stays hand-merged with upstream when it changes.
               "value.c"}

# Upstream lines behind in a pinned file stop being news at about this many;
# below it the file is just locally edited.
LAG_WARN = 40

# Upstream src/ files that are core-specific by construction.  lang/ is the
# interpreter front end only: the native/LLVM emitters, the LSP/MCP/serve
# bridges, the libFuzzer driver and the runtime shim belong to lume-core's
# CLI, not to the host build.  Naming them explicitly is what lets
# unclassified_upstream() treat every *other* new upstream source as news.
CORE_ONLY_PREFIXES = ("backend", "codegen", "llvm_", "irbuf", "fuzz_",
                      "bridge_lsp", "bridge_mcp", "bridge_native", "bridge_serve")
CORE_ONLY_NAMES = {"rt.c", "server.h", "sql_guard_internal.h"}

GREEN = "\033[32m"
YELLOW = "\033[33m"
RED = "\033[31m"
OFF = "\033[0m"


def gh_error(msg):
    """Emit a GitHub Actions error annotation when running in CI so the sync
    gate failures surface inline in PR diffs instead of only in the collapsed
    step log. Harmless plain text outside Actions."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print("::error::%s" % msg)


def pin_core_dir():
    """The core_dir line in lang/PIN, resolved relative to the repo root.

    This field used to be dead: write_pin() emitted it and nothing ever read
    it, so the tree was always found via DEFAULT_CORE.  Make it the documented
    location (LUME_CORE_DIR still wins) so a differently named sibling repo
    can be expressed in PIN instead of silently falling back.
    """
    if not os.path.isfile(PIN):
        return None
    with open(PIN, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition(":")
            if key.strip() == "core_dir":
                value = value.strip()
                if not value:
                    return None
                if not os.path.isabs(value):
                    value = os.path.normpath(os.path.join(ROOT, value))
                return value
    return None


def core_dir():
    d = os.environ.get("LUME_CORE_DIR") or pin_core_dir() or DEFAULT_CORE
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


def unclassified_upstream(files, core):
    """Upstream language sources nobody has classified yet.

    lang_sources() reads `git ls-files lang`, so a source file added upstream
    is structurally invisible: sync never copies it, write_manifest never
    baselines it and check never reports it as drift.  Everything core-only
    is named in CORE_ONLY_*, so a genuinely new front-end file has nowhere
    to hide.  It is a report, not an auto-copy: whether a new upstream file
    belongs in lang/ is a human decision (followed by `make sync-lang`).
    """
    if core is None:
        return []
    src = os.path.join(core, "src")
    if not os.path.isdir(src):
        return []
    # `files` comes from `git ls-files lang`; a freshly created but not yet
    # staged file would look "absent" and produce a bogus UNCLASSIFIED.  The
    # question is whether the file exists in lang/, not whether it is tracked.
    have = set(files)
    have |= {n for n in os.listdir(LANG)
             if n.endswith((".c", ".h")) and os.path.isfile(os.path.join(LANG, n))}
    out = []
    for name in sorted(os.listdir(src)):
        if not name.endswith((".c", ".h")) or name in have:
            continue
        if name in CORE_ONLY_NAMES or name.startswith(CORE_ONLY_PREFIXES):
            continue
        if os.path.isfile(os.path.join(src, name)):
            out.append(name)
    return out


def report_unclassified(files, core):
    unknown = unclassified_upstream(files, core)
    if not unknown:
        return False
    for f in unknown:
        print("%sUNCLASSIFIED%s upstream src/%s: not in lang/ and not named "
              "core-only" % (RED, OFF, f))
    gh_error("sync-lang: %d unclassified upstream source file(s); classify "
             "before sync" % len(unknown))
    print("         either sync it into lang/ (then `make sync-lang`), or add it "
          "to CORE_ONLY_NAMES / CORE_ONLY_PREFIXES in scripts/sync-lang.py")
    return True


def dirty_synced(files):
    """Synced files that currently carry uncommitted edits.

    A synced file is the host's copy of an upstream file, so `sync` is
    allowed to replace it wholesale - which is exactly why it must never do
    so while someone has unstaged work in it.  Uncommitted edits are not
    recoverable from the index, so overwriting them loses the work with no
    trail except this script's report.
    """
    out = subprocess.run(["git", "-C", ROOT, "diff", "--name-only", "--"] +
                         ["lang/" + f for f in files],
                         capture_output=True, text=True)
    return {os.path.basename(x) for x in out.stdout.split()}


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
    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as fh:
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
    # Materialise it first: unified_diff() returns a one-shot generator, so
    # the second sum() used to iterate an exhausted iterator and report
    # `ours == 0` for every file. That silently erased host-side lines from
    # the lag note (and from accept-lag's report) without changing `up`.
    diff = list(difflib.unified_diff(a, b, lineterm=""))
    up = sum(1 for x in diff if x.startswith("-") and not x.startswith("---"))
    ours = sum(1 for x in diff if x.startswith("+") and not x.startswith("+++"))
    return up, ours


LAG = os.path.join(LANG, "PIN.lag")


def read_lag():
    """{filename: (accepted line count, accepted upstream md5)} from PIN.lag.

    A host-owned file is behind upstream because the two trees genuinely
    fork: lume links agent-httpd and seeds its own registry, lume-core
    links neither.  Counting those lines every time is a report that is
    always wrong in the same direction, so what is accepted gets written
    down once with `sync-lang.py accept-lag` and only *growth* past that
    number reads as news.

    The md5 is the second axis.  unified_diff(a=upstream, b=ours) reports an
    upstream *deletion* as `+` lines on our side, so `up` stays flat while
    the host keeps dead code: the count alone cannot see it.  Two-column
    (legacy) PIN.lag lines read back with an empty hash, which disables the
    hash leg until the next `accept-lag` upgrades them.
    """
    out = {}
    if not os.path.isfile(LAG):
        return out
    with open(LAG, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, rest = line.partition(" ")
            parts = rest.split()
            if name and parts:
                try:
                    out[name] = (int(parts[0]),
                                 parts[1] if len(parts) > 1 else "")
                except ValueError:
                    pass
    return out


def write_lag():
    """Accept today's upstream line count AND content for host-owned files."""
    core = core_dir()
    # newline="\n" is not cosmetic: on Windows the platform default would
    # translate every \n to \r\n, and the committed PIN.lag (LF) would show
    # up as a whole-file diff after every accept-lag.
    with open(LAG, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# accepted upstream state for each host-owned file.\n")
        fh.write("# written by `sync-lang.py accept-lag`.  Columns:\n")
        fh.write("#   name  upstream-only-line-count  upstream-file-md5\n")
        fh.write("# check fails when the count grows OR the md5 changes.\n")
        for f in sorted(HOST_PINNED):
            d = upstream_delta(f)
            if d:
                p = os.path.join(core, "src", f)
                digest = md5(p) if os.path.isfile(p) else "-"
                fh.write("%s %d %s\n" % (f, d[0], digest))


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
         datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
         " ".join(sorted(HOST_PINNED)) or "-")
    with open(PIN, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)


def host_registry_gaps():
    """Builtins the host seeds at runtime but its typechecker cannot name.

    A partial hand-merge of typecheck.c is invisible to both md5 and line-lag
    checks: interp.c's registry and the arity table get the new name while
    LUME_BUILTIN_NAMES[] does not, so every script using it fails --check
    even though the runtime would have handled it (crypt_verify and
    const_time_eq were exactly this).  The host's own interp registry is the
    reference, not upstream's name table: the host deliberately implements
    fewer builtins than lume-core.  Only registry - names is a gap; the
    reverse direction holds type words and verb groups by design.
    """
    tc_path = os.path.join(LANG, "typecheck.c")
    ip_path = os.path.join(LANG, "interp.c")
    if not (os.path.isfile(tc_path) and os.path.isfile(ip_path)):
        return []
    with open(tc_path, encoding="utf-8") as fh:
        tc = fh.read()
    m = re.search(r"LUME_BUILTIN_NAMES\[\]\s*=\s*\{(.*?)\};", tc, re.S)
    if not m:
        return []
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", m.group(1), flags=re.S)
    names = set(re.findall(r'"([^"]+)"', body))
    with open(ip_path, encoding="utf-8") as fh:
        ip = fh.read()
    registered = set(re.findall(r'\{\s*"([a-z_][a-z0-9_]*)"\s*,\s*b_\w+\s*\}', ip))
    return sorted(registered - names)


def main():
    argv = sys.argv[1:]
    force = "--force" in argv
    strict = "--strict" in argv
    mode = next((a for a in argv if not a.startswith("-")), "check")
    if mode not in ("sync", "check", "accept-lag"):
        sys.exit("usage: sync-lang.py {sync|check|accept-lag} [--force]")

    files = lang_sources()

    if mode == "check":
        # This one is host-internal, so it must run even when lume-core is
        # not reachable and check falls back to the md5 baseline.
        gaps = host_registry_gaps()
        if gaps:
            for g in gaps:
                print("%sREGISTRY%s %s is seeded in lang/interp.c but missing "
                      "from LUME_BUILTIN_NAMES[] (partial hand-merge)" % (RED, OFF, g))
            print("%d builtin(s) would fail --check; finish the hand-merge in "
                  "lang/typecheck.c" % len(gaps))
            return 1

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
            # LUME_REQUIRE_CORE=1 is what CI sets.  Without it, a broken
            # sibling checkout (wrong path, failed checkout step, renamed
            # repo) degrades this "strict" gate into an md5-only check that
            # would never notice upstream drift, yet still returns green.
            if os.environ.get("LUME_REQUIRE_CORE") == "1":
                gh_error("sync-lang: strict check requires a sibling "
                         "lume-core tree (LUME_REQUIRE_CORE=1)")
                print("%sFAIL%s check requires a sibling lume-core tree "
                      "(LUME_REQUIRE_CORE=1) but none was found at %s"
                      % (RED, OFF, os.environ.get("LUME_CORE_DIR")
                         or pin_core_dir() or DEFAULT_CORE))
                return 1
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
            if not os.path.isfile(ls) or md5(cs) != md5(ls):
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
            entry = accepted.get(f)
            base = entry[0] if entry else None
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
                continue
            # An upstream deletion shows up as `+` lines on OUR side, so
            # `up` does not grow: the host silently keeps dead code forever.
            # The accepted md5 is what catches that (and any net-zero rewrite).
            prev = entry[1] if len(entry) > 1 else ""
            cur = md5(os.path.join(core, "src", f))
            if prev and cur != prev:
                lagging.append(f)
                print("%sBEHIND%s     %-18s upstream content changed with no "
                      "line growth (md5 %s -> %s); hand-merge, then "
                      "`accept-lag`" % (YELLOW, OFF, f, prev[:8], cur[:8]))
            else:
                print("host-owned  %-18s %s (accepted)" % (f, note))

        for f in drift:
            print("%sDRIFT%s   %s" % (RED, OFF, f))
        if drift:
            gh_error("sync-lang: %d of %d synced file(s) differ from core; "
                     "run make sync-lang" % (len(drift),
                                            len(files) - len(HOST_PINNED)))
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
        hard = []
        if report_unclassified(files, core):
            hard.append("unclassified upstream source file(s)")
        state, pin_note, suffix = pin_status(core, sha)
        if state in ("ahead", "missing", "diverged", "none"):
            print("%sSTALEPIN%s %s - run `make sync-lang`"
                  % (YELLOW, OFF, pin_note))
            gh_error("sync-lang: STALEPIN %s - run make sync-lang" % pin_note)
            hard.append("STALEPIN: %s" % pin_note)
        elif pin_note:
            # Green, but say why the pin is not HEAD anyway.  "folded" is a
            # note, not a failure: core moved on without touching synced files.
            print("pin         %s" % pin_note)
        print("%sok%s lang/ matches %s @ %s - %d synced, %d host-owned, %d host-only%s"
              % (GREEN, OFF, os.path.basename(core), sha[:12],
                 len(files) - len(HOST_PINNED), len(HOST_PINNED), len(host_only),
                 suffix))
        if lagging:
            # `lagging` covers both axes now: more upstream-only lines AND
            # a changed upstream md5 with no line growth.  Do not call it
            # "grew" - that made an upstream deletion look like a miscount.
            print("%d host-owned file(s) are behind the accepted lang/PIN.lag "
                  "baseline (extra upstream lines or changed content); "
                  "a hand-merge is owed" % len(lagging))
            hard.append("%d host-owned file(s) changed past lang/PIN.lag"
                        % len(lagging))
        if strict and hard:
            gh_error("sync-lang: %d host-owned file(s) changed past "
                     "lang/PIN.lag" % len(lagging))
            for h in hard:
                print("%sFAIL%s %s" % (RED, OFF, h))
            print("check --strict: resolve the above, then re-run; a deliberate "
                  "new lag needs `sync-lang.py accept-lag` after the hand-merge")
            return 1
        return 0

    if mode == "accept-lag":
        core_dir()          # upstream_delta reads through the core tree
        write_lag()
        print("%sok%s accepted the current upstream line count and md5 for %d "
              "host-owned file(s) in lang/PIN.lag; check now reports growth "
              "or any content change past it" % (GREEN, OFF, len(HOST_PINNED)))
        return 0

    core = core_dir()
    if report_unclassified(files, core):
        gh_error("sync-lang: classify the upstream file(s) above before "
                 "re-pinning lang/")
        print("%sREFUSED%s classify the upstream file(s) above before "
              "re-pinning lang/." % (RED, OFF))
        return 1
    sha = core_sha(core)
    # Replacing a synced file wholesale is the point of `sync`, so it must
    # not do it while someone has unstaged work in the file: uncommitted
    # edits live only in the worktree, and overwriting them loses them with
    # nothing left behind but this report.  lang/builtins_http.c lost its
    # HTTP_DBG macro this way, which is the reason for the guard.
    # Only files sync would actually replace count: a host-owned file is
    # never overwritten, so edits in it are none of sync's business.
    dirty = set() if force else dirty_synced(synced_files(files))
    changed = []
    for f in sorted(dirty):
        print("%sSKIP%s      %-18s has uncommitted edits; `--force` overwrites"
              % (YELLOW, OFF, f))
    if dirty:
        print("%sREFUSED%s updating lang/PIN: %d synced file(s) still carry "
              "uncommitted edits." % (RED, OFF, len(dirty)))
        print("         Commit them, hand-merge, or run `sync-lang.py sync "
              "--force` to drop them.")
        return 1
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
