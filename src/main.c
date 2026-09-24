#include "lume.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#endif

/* Lume entry point: parse + type check + interpret a script, then hand the
 * process over to agenthttpd_run() when the script calls run(). */

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--check|--dump|--watch] <script.lume>\n"
            "  --check   parse + type check (no side effects)\n"
            "  --dump    parse and dump the AST, then exit\n"
            "  --watch   dev hot reload: validate + restart the server child on\n"
            "            every edit of <script.lume> (SIGUSR1 = force reload);\n"
            "            invalid edits keep the old server running\n"
            "\n"
            "The script is Lume source: type/struct declarations, server {},\n"
            "route/tool declarations plus expressions with static type\n"
            "checking; run() starts the embedded agent-httpd server\n"
            "(libagenthttpd.a).\n",
            prog);
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n && n != 0) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    if (len_out) *len_out = (size_t)n;
    return buf;
}

/* ---------- --watch: dev hot reload ------------------------------------
 *
 * The serving process cannot swap its VM in place: routes/tools are
 * registered into two pre-fork tables (agent-httpd g_routes + VM routes[])
 * and `g_framework_started` seals registration once run() begins. So hot
 * reload works at process level instead:
 *
 *   parent (watcher)                 child (serving process)
 *   ┌────────────────────┐           ┌──────────────────────┐
 *   │ kqueue(VNODE)/poll │  edit     │ bin/lume <script>    │
 *   │ self-pipe (SIGUSR1)│ ───────►  │  parse+typecheck+run │
 *   │ validate new edit  │           │  agenthttpd_run      │
 *   │ SIGTERM child      │◄───────── │  (graceful shutdown) │
 *   │ fork+exec new child│  ~350ms   └──────────────────────┘
 *   └────────────────────┘  debounce
 *
 * Invalid edits are reported and the old child keeps serving; a valid edit
 * restarts the child (port is freed by the child's graceful shutdown
 * before the next child binds). SIGUSR1 forces the same restart path. */

#define WATCH_POLL_MS       500
#define WATCH_DEBOUNCE_MS   350
#define WATCH_STOP_GRACE_MS 3000

static volatile sig_atomic_t g_watch_quit = 0;
static volatile sig_atomic_t g_watch_usr1 = 0;
static int g_watch_pipe[2] = {-1, -1};

static void watch_signal(int sig) {
    if (sig == SIGUSR1) g_watch_usr1 = 1;
    else g_watch_quit = 1;
    if (g_watch_pipe[1] >= 0) {
        ssize_t w = write(g_watch_pipe[1], "x", 1);
        (void)w;
    }
}

/* parse + type check without running; err[] gets the first failure. */
static bool watch_validate(const char *script, char *err, size_t errsz) {
    size_t len = 0;
    char *source = read_file(script, &len);
    if (!source) {
        snprintf(err, errsz, "cannot read %s", script);
        return false;
    }
    Node *prog = parse_program(source, err, errsz);
    if (!prog) { free(source); return false; }
    bool ok = type_check_program(prog, err, errsz);
    /* AST leak per validation is intentional: dev tool, parser has no
     * node_free and one reload = one program tree. */
    free(source);
    return ok;
}

static void file_sig(const char *path, struct timespec *mtime, long long *size) {
    struct stat st;
    if (stat(path, &st) != 0) { mtime->tv_sec = -1; *size = -1; return; }
#ifdef __APPLE__
    *mtime = st.st_mtimespec;
#else
    mtime->tv_sec = st.st_mtim.tv_sec;
    mtime->tv_nsec = st.st_mtim.tv_nsec;
#endif
    *size = (long long)st.st_size;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* SIGTERM, wait up to WATCH_STOP_GRACE_MS, escalate to SIGKILL. */
static void stop_child(pid_t child) {
    if (child <= 0) return;
    kill(child, SIGTERM);
    int status = 0;
    long long deadline = now_ms() + WATCH_STOP_GRACE_MS;
    for (;;) {
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) return;
        if (now_ms() >= deadline) break;
        usleep(50 * 1000);
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
}

static pid_t start_child(const char *argv0, const char *script) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("[watch] fork"); return -1; }
    if (pid == 0) {
        /* child: fresh address space via exec; watcher fds must not leak */
        if (g_watch_pipe[0] >= 0) close(g_watch_pipe[0]);
        if (g_watch_pipe[1] >= 0) close(g_watch_pipe[1]);
        execl(argv0, argv0, script, (char *)NULL);
        fprintf(stderr, "[watch] exec %s failed: %s\n", argv0, strerror(errno));
        _exit(127);
    }
    printf("[watch] server child pid=%d (script=%s)\n", pid, script);
    fflush(stdout);
    return pid;
}

static int run_watch(const char *argv0, const char *script) {
    struct timespec sigmask_mtime;
    long long sigsize = -1;
    file_sig(script, &sigmask_mtime, &sigsize);
    if (sigsize < 0) {
        fprintf(stderr, "lume: cannot read %s\n", script);
        return 1;
    }

    char err[512] = {0};
    if (!watch_validate(script, err, sizeof(err))) {
        fprintf(stderr, "lume: %s\n", err);
        return 1;
    }

    if (pipe(g_watch_pipe) != 0) { perror("[watch] pipe"); return 1; }
    fcntl(g_watch_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_watch_pipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = watch_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int kq = -1, watch_fd = -1;
#if defined(__APPLE__)
    kq = kqueue();
    if (kq >= 0) {
        struct kevent ev;
        EV_SET(&ev, (uintptr_t)g_watch_pipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
        kevent(kq, &ev, 1, NULL, 0, NULL);
        watch_fd = open(script, O_RDONLY | O_EVTONLY);
        if (watch_fd >= 0) {
            EV_SET(&ev, (uintptr_t)watch_fd, EVFILT_VNODE,
                   EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_DELETE | NOTE_RENAME |
                                     NOTE_EXTEND | NOTE_ATTRIB,
                   0, NULL);
            kevent(kq, &ev, 1, NULL, 0, NULL);
        }
    }
#endif

    printf("[watch] watching %s (SIGUSR1 = force reload, Ctrl+C = stop)\n",
           script);
    fflush(stdout);

    pid_t child = start_child(argv0, script);
    long long last_change = now_ms();
    long long last_restart = 0;
    long long last_msg = 0;
    int pending = 0;

    while (!g_watch_quit) {
        /* 1) file content changed? (ground truth: stat, not just events —
         *    editors rename/replace the inode under us) */
        struct timespec mt;
        long long sz;
        file_sig(script, &mt, &sz);
        if (sz >= 0 && (mt.tv_sec != sigmask_mtime.tv_sec ||
                        mt.tv_nsec != sigmask_mtime.tv_nsec || sz != sigsize)) {
            sigmask_mtime = mt;
            sigsize = sz;
            last_change = now_ms();
            pending = 1;
        }

        /* 2) act on a pending change / SIGUSR1 (debounced) */
        long long now = now_ms();
        if ((pending || g_watch_usr1) && now - last_change >= WATCH_DEBOUNCE_MS &&
            now - last_restart >= WATCH_DEBOUNCE_MS) {
            g_watch_usr1 = 0;
            pending = 0;
            last_change = now;
            if (!watch_validate(script, err, sizeof(err))) {
                if (now - last_msg > 2000) {
                    fprintf(stderr,
                            "[watch] invalid edit, old server keeps running:\n"
                            "  %s\n", err);
                    last_msg = now;
                }
            } else {
                stop_child(child);
                last_restart = now_ms();
                child = start_child(argv0, script);
            }
        }

        /* 3) reap an unexpectedly dead child (crash / run() failure) */
        if (child > 0) {
            int status = 0;
            pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child) {
                if (!g_watch_quit && now - last_msg > 1000) {
                    fprintf(stderr,
                            "[watch] child pid=%d exited (status %d); waiting "
                            "for next edit or SIGUSR1\n",
                            (int)child, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                    last_msg = now;
                }
                child = 0;
            }
        }

        /* 4) sleep until the next event / file touch / poll tick */
#if defined(__APPLE__)
        if (kq >= 0) {
            struct kevent ev;
            struct timespec to = {0, WATCH_POLL_MS * 1000000L};
            int n = kevent(kq, NULL, 0, &ev, 1, &to);
            if (n > 0 && ev.filter == EVFILT_READ) {
                char drain[16];
                while (read(g_watch_pipe[0], drain, sizeof(drain)) > 0) { }
            } else if (n > 0 && ev.filter == EVFILT_VNODE) {
                /* re-arm on the (possibly replaced) file so NOTE_* keeps firing */
                close(watch_fd);
                watch_fd = open(script, O_RDONLY | O_EVTONLY);
                if (watch_fd >= 0) {
                    struct kevent reg;
                    EV_SET(&reg, (uintptr_t)watch_fd, EVFILT_VNODE,
                           EV_ADD | EV_CLEAR,
                           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND |
                           NOTE_ATTRIB,
                           0, NULL);
                    kevent(kq, &reg, 1, NULL, 0, NULL);
                }
            }
        } else {
            usleep(WATCH_POLL_MS * 1000);
        }
#else
        usleep(WATCH_POLL_MS * 1000);
#endif
    }

    printf("[watch] stopping child pid=%d\n", (int)child);
    fflush(stdout);
    stop_child(child);
    if (watch_fd >= 0) close(watch_fd);
    if (kq >= 0) close(kq);
    close(g_watch_pipe[0]);
    close(g_watch_pipe[1]);
    return 0;
}

int main(int argc, char **argv) {
    bool do_check = false;
    bool do_dump = false;
    bool do_watch = false;
    const char *script = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) do_check = true;
        else if (strcmp(argv[i], "--dump") == 0) do_dump = true;
        else if (strcmp(argv[i], "--watch") == 0) do_watch = true;
        else if (argv[i][0] != '-') script = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!script) { usage(argv[0]); return 2; }

    if (do_watch) return run_watch(argv[0], script);

    size_t len = 0;
    char *source = read_file(script, &len);
    if (!source) {
        fprintf(stderr, "lume: cannot read %s\n", script);
        return 1;
    }

    char err[512] = {0};
    Node *prog = parse_program(source, err, sizeof(err));
    if (!prog) {
        fprintf(stderr, "lume: %s\n", err[0] ? err : "parse error");
        free(source);
        return 1;
    }

    if (do_dump) {
        node_print(prog, 0);
        free(source);
        return 0;
    }

    /* Static type checking always runs: Lume is strongly typed at
     * compile time. --check stops here. */
    if (!type_check_program(prog, err, sizeof(err))) {
        fprintf(stderr, "lume: %s\n", err[0] ? err : "type error");
        free(source);
        return 1;
    }

    if (do_check) {
        printf("parse OK (%zu bytes)\n", len);
        free(source);
        return 0;
    }

    VM vm;
    vm_init(&vm);
    bridge_init(&vm);

    exec_program(&vm, prog);
    if (vm.error) {
        fprintf(stderr, "lume: %s\n", vm.error_msg);
        free(source);
        return 1;
    }

    /* run() hands the process to agent-httpd. A script that never calls it
     * is a pure language program (compute + print) — that is fine, just not
     * a server. */
    if (!vm.run_called)
        fprintf(stderr, "lume: note: script completed without run()\n");
    free(source);
    return 0;
}