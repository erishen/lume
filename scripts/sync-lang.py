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
"""
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG = os.path.join(ROOT, "lang")
PIN = os.path.join(LANG, "PIN")
DEFAULT_CORE = os.path.join(os.path.dirname(ROOT), "lume-core")

# Files the host owns and must never be replaced by the upstream copy:
#
# lume.h  pulls in agenthttpd.h (tools/skills/sqlite_tool/minijson) - the
#         host-only integration lume-core dropped for a local sbuf.h.
# main.c  is the CLI entry point; the core version includes backend.h /
#         backend_llvm.h / codegen.h, which belong to the native backends
#         that lume has no business linking (agent-httpd owns it instead).
#         Copying it over breaks the build with "'backend.h' file not found".
# builtins_internal.h  defines ToolDef with its own TOOL_MAX; agent-httpd's
# tools.h defines the same tag with 96.  lume-core never links agent-httpd so
# it never saw the clash - here it is an immediate redefinition error.
# builtins_internal.h  defines ToolDef with its own TOOL_MAX; agent-httpd's
# tools.h defines the same tag with 96.  lume-core never links agent-httpd so
# it never saw the clash - here it is an immediate redefinition error.
# builtins.c  routes the HTTP builtins through native_http_get(), which lume-
# core defines in builtins_http.c - a file with no business in this tree.
# builtins_internal.h  defines ToolDef with its own TOOL_MAX; agent-httpd's
# tools.h defines the same tag with 96.  lume-core never links agent-httpd so
# it never saw the clash - here it is an immediate redefinition error.
# builtins.c, builtins_catalog.c and builtins_fs.c all pull that header in
# and reach for ToolDef.path / SKILL_MAX, so they ride with it.
HOST_PINNED = {"lume.h", "main.c", "builtins_internal.h", "builtins.c",
               "builtins_catalog.c", "builtins_fs.c", "vdom.c",
               # interp.c's registry seeds b_http_get; lume-core implements it
               # in builtins_http.c, which drags OpenSSL into a host link that
               # never asked for it.  The host seeds its own registry instead.
               "interp.c", "builtins.h",
               # the checker resolves names against the seeded registry, and
               # this host seeds sql_query / sql_write on top of core's.  The
               # upstream checker rejects them as undefined.
               "typecheck.c", "typecheck_expr.c", "typecheck_stmt.c"}

GREEN = "\033[32m"
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


def write_pin(sha):
    body = (
        "# lang/ is a pinned copy of the lume-core language tree.\n"
        "# Bump it with:  make sync-lang && make check-sync\n"
        "# Detect drift without touching anything:  make check-sync\n"
        "#\n"
        "# Everything under host_owned is never overwritten: each one exists\n"
        "# because lume and lume-core genuinely diverged (agenthttpd\n"
        "# integration, the native-backend CLI, the ToolDef tag, the seeded\n"
        "# sql_* builtins).  A bump may still need a hand-merge in them.\n"
        "# The core-specific files (codegen*/backend*/llvm_codegen*/rt.c) are\n"
        "# not copied at all - lume has no native backend, agent-httpd owns it.\n"
        "core_dir: ../lume-core\n"
        "sha: %s\n"
        "synced_at: %s\n"
        "host_owned: %s\n"
    ) % (sha, subprocess.run(["date", "+%Y-%m-%d %H:%M:%S"],
                             capture_output=True, text=True).stdout.strip(),
         " ".join(sorted(HOST_PINNED)) or "-")
    with open(PIN, "w", encoding="utf-8") as fh:
        fh.write(body)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    if mode not in ("sync", "check"):
        sys.exit("usage: sync-lang.py {sync|check}")

    core = core_dir()
    sha = core_sha(core)
    files = lang_sources()

    if mode == "check":
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
        for f in drift:
            print("%sDRIFT%s   %s" % (RED, OFF, f))
        if drift:
            print("%d of %d synced file(s) differ from core; run `make sync-lang`"
                  % (len(drift), len(files) - len(HOST_PINNED)))
            return 1
        print("%sok%s lang/ matches %s @ %s - %d synced, %d host-owned, %d host-only"
              % (GREEN, OFF, os.path.basename(core), sha[:12],
                 len(files) - len(HOST_PINNED), len(HOST_PINNED), len(host_only)))
        return 0

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
    write_pin(sha)
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
