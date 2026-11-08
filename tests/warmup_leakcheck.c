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
 * Why repeating the call is the meaningful check: lume_http_warmup() now
 * does one getaddrinfo() on an external name and pre-builds the static
 * business SSL_CTX. The addrinfo chain must be freed on every round (or
 * LSan sees a leak growing with ROUNDS), and the SSL_CTX must be created
 * once, not once per round. The 2026-11-07 build also ran an in-memory
 * BIO-pair handshake with nine owned OpenSSL objects behind a shared
 * `done:` label; that code was removed 11-08 after A/B showed DNS priming
 * alone fixes the fork guard. This harness now covers the smaller remaining
 * allocation surface, with the same negative control against a false green.
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
