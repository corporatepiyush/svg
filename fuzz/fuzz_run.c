/* fuzz_run.c - Batch runner for fuzzing svg2img.
 *
 * Reads a list of SVG paths on stdin, renders each in a forked child, and
 * reports any child that dies from a signal or exits with an unexpected
 * status.  Forking per input means a crash in the parser cannot take down
 * the driver, and the parent can keep going over millions of inputs.
 *
 * Build:
 *   cc -std=c17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
 *      -Ilibs -o fuzz_run fuzz_run.c libs/xml_parse.c libs/xml_dom.c \
 *      libs/path_parse.c libs/path_util.c libs/color.c libs/length.c \
 *      libs/svg_*.c libs/raster_*.c libs/png_*.c libs/font.c -lm
 *
 * Usage:
 *   ./fuzz_run < list.txt
 */
#define _GNU_SOURCE
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

static long n_ok = 0, n_crash = 0, n_bad_status = 0, n_timeout = 0, n_empty = 0;

#define FUZZ_TIMEOUT_SECONDS 60

static void on_alarm(int sig) {
    (void)sig;
    _exit(3);   /* parent reports this as TIMEOUT */
}
static double t_start;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static char* read_file(const char* path, size_t* out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 1 << 16, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
        ssize_t got = read(fd, buf + len, cap - len - 1);
        if (got < 0) { free(buf); close(fd); return NULL; }
        if (got == 0) break;
        len += (size_t)got;
    }
    close(fd);
    buf[len] = 0;
    *out_len = len;
    return buf;
}

/* Render one file.  Returns 0 on success, non-zero on failure. */
static int render_one(const char* path, int aa) {
    size_t len = 0;
    char* data = read_file(path, &len);
    if (!data) return 10;
    svg2img_options o;
    memset(&o, 0, sizeof(o));
    o.aa = aa;
    o.scale = 1.0f;
    int rc = svg2img_convert(data, len, "/dev/null", &o);
    free(data);
    return rc;
}

int main(int argc, char** argv) {
    int aa = (argc > 1) ? atoi(argv[1]) : 2;
    int max_bytes = (argc > 2) ? atoi(argv[2]) : 0;   /* 0 = unlimited */
    (void)max_bytes;

    char line[4096];
    t_start = now_sec();

    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;

        /* Reject absurdly large inputs early: they are usually the slow
         * path, not the interesting one, and they dominate wall time. */
        struct stat st;
        if (stat(line, &st) == 0 && st.st_size > (off_t)(8 << 20)) {
            n_bad_status++;
            continue;
        }

        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            /* Child: silence sanitizer output unless it finds something.
             * The alarm stops a single pathological input from occupying a
             * worker forever; the parent reports exit code 3 as TIMEOUT. */
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, 1); close(devnull); }
            signal(SIGALRM, on_alarm);
            int secs = FUZZ_TIMEOUT_SECONDS;
            const char* env = getenv("SVG2IMG_FUZZ_TIMEOUT");
            if (env && atoi(env) > 0) secs = atoi(env);
            alarm((unsigned)secs);
            int rc = render_one(line, aa);
            _exit(rc == 0 ? 0 : 2);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            n_crash++;
            printf("CRASH %s signal=%d\n", line, WTERMSIG(status));
            fflush(stdout);
        } else if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) n_ok++;
            else if (code == 2) n_bad_status++;   /* clean conversion failure */
            else if (code == 3) {
                n_timeout++;
                printf("TIMEOUT %s\n", line);
                fflush(stdout);
            } else {
                n_crash++;
                printf("ABNORMAL %s exit=%d\n", line, code);
                fflush(stdout);
            }
        }

        if ((n_ok + n_crash + n_bad_status) % 5000 == 0) {
            double el = now_sec() - t_start;
            fprintf(stderr, "[%ld ok, %ld crash, %ld fail] %.0f inputs/s\n",
                    n_ok, n_crash, n_bad_status, (double)(n_ok + n_crash) / (el > 0 ? el : 1));
        }
    }

    double el = now_sec() - t_start;
    fprintf(stderr, "\ndone: %ld ok, %ld crashes, %ld timeouts, "
            "%ld clean-failures in %.1fs\n",
            n_ok, n_crash, n_timeout, n_bad_status, el);
    return (n_crash || n_timeout) ? 1 : 0;
}