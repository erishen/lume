/*
 * Dedicated ASan/LSan harness for lume_http_warmup() (lang/builtins_http.c).
 *
 * `make asan` has to run with detect_leaks=0: the interpreter keeps Type
 * objects for the life of the process, so a global leak check reports them
 * even though that retention is the design. That blanket exemption also
 * hides the warmup path. This binary links the same objects but never
 * initialises the interpreter -- it only calls lume_http_warmup() -- so
 * LeakSanitizer can be enabled here in isolation, with no AST/type noise.
 *
 * Why the success path is the meaningful one: lume_http_warmup() has a
 * single shared `done:` cleanup label, and every early exit jumps to it.
 * A successful round is the worst case -- all nine owned pointers (cli,
 * srv, cbio, sbio, cctx, sctx, cert, key, pctx) are non-NULL, so every
 * free() in that block executes. An early exit is a strict subset of that
 * same block (the not-yet-created pointers are NULL and skipped), and no
 * branch allocates an owning pointer outside those nine locals: the
 * X509_NAME and X509_STORE handles are borrowed, not owned. Repeating the
 * call also catches accumulation -- the business SSL_CTX is static and
 * must be created once, not once per round.
 *
 * LeakSanitizer ships with the Linux ASan runtime only (Apple clang has no
 * LSan), so on macOS this degrades to an ASan/UBSan exercise of the same
 * rounds. The authoritative leak gate is CI's ubuntu `asan` job.
 *
 * LUME_WARMUP_SELFTEST_LEAK builds this same file as a standalone negative
 * control: a process that leaks on purpose and MUST exit non-zero. The
 * Makefile asserts that before trusting the real harness, so a disabled or
 * broken LeakSanitizer can never produce a false green.
 */
#include <stdio.h>
#include <stdlib.h>

#if defined(LUME_WARMUP_HAS_LSAN)
#  include <sanitizer/lsan_interface.h>
#endif

#if defined(LUME_WARMUP_SELFTEST_LEAK)

/* Built with -O0 so the allocation is not optimised away. LSan's atexit
 * check must report the unreachable block and force a non-zero exit. */
int main(void) {
    (void)malloc(4096);
    return 0;
}

#else

int lume_http_warmup(void);

#define ROUNDS 8

int main(void) {
    for (int i = 1; i <= ROUNDS; i++) {
        int rc = lume_http_warmup();
        if (rc != 0) {
            fprintf(stderr, "FAIL warmup round %d: rc=%d\n", i, rc);
            return 1;
        }
#if defined(LUME_WARMUP_HAS_LSAN)
        /* Recoverable so one round's finding becomes a clear test failure
         * instead of an ASan abort with no context. */
        if (__lsan_do_recoverable_leak_check() != 0) {
            fprintf(stderr, "FAIL warmup round %d: LSan reported leaks\n", i);
            return 1;
        }
#endif
    }
    printf("ok   warmup leakcheck: %d rounds, no leaks\n", ROUNDS);
    return 0;
}

#endif /* LUME_WARMUP_SELFTEST_LEAK */
