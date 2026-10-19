/*
 * Open-loop HTTP load generator for the CCMP25 resilience measurement
 * (studies/ccmp25-board/RESILIENCE_TEST.md). It measures how the gateway
 * behaves above saturation on the user's own board.
 *
 * Design:
 * - Each worker thread owns a fixed per-second send schedule (rate/threads),
 *   paced by the monotonic clock and independent of how fast replies arrive.
 * - One TCP connection per request; the request bytes are read from a file.
 * - The client reads each reply to EOF so the board closes the connection
 *   first (avoids host TIME_WAIT exhausting ephemeral ports), and checks the
 *   reply's status line against --expect.
 * - A separate "legitimate" thread issues one --legit request every 100 ms
 *   and records status and latency (this is the signal of interest).
 *
 * Safety (this tool is deliberately conservative):
 * - Refuses an offered rate above 1000 req/s unless --direct-link is given.
 * - Refuses if the estimated wire rate would exceed --max-mbps.
 * - If no bytes, connection, or reset came back from the board for
 *   --dead-ms while requests were outstanding, the whole run stops: a board
 *   that has stopped answering must not keep being contacted.
 *
 * One JSON document (per-second timeline + totals) is written to stdout.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define MAX_SECONDS 1200

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- shared configuration and state ---- */
struct cfg {
    struct sockaddr_in addr;
    const uint8_t *req;
    size_t req_len;
    int expect_status;   /* status line checked against this; 0 = any */
    int threads;
    double rate;         /* total offered requests per second */
    int duration_s;
    int conn_timeout_ms; /* per-request connect+send+recv budget */
    uint64_t t0;
};

struct counters {
    _Atomic uint64_t attempted, completed, connect_fail, io_error, timeout;
    _Atomic uint64_t status_expected, status_other, status_2xx; /* 2xx = wrong allow on a deny flood */
    _Atomic uint64_t last_progress_ns;                          /* any sign of life from the board */
};

/* Per-second legitimate-client samples. */
struct legit_sample {
    int status;
    double latency_ms;
};

static struct cfg CFG;
static struct counters CTR;
static volatile sig_atomic_t g_stop;
static _Atomic uint64_t g_outstanding;


/* One request: connect, send, read to EOF, classify. Returns 0 on a clean
 * reply, negative on connect/io/timeout. Updates last_progress_ns on any
 * byte, reset, or completed connection from the board. */
static int do_request(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        atomic_fetch_add(&CTR.connect_fail, 1);
        return -1;
    }
    struct timeval tv = { .tv_sec = CFG.conn_timeout_ms / 1000,
                          .tv_usec = (CFG.conn_timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    atomic_fetch_add(&g_outstanding, 1);
    atomic_fetch_add(&CTR.attempted, 1);
    int rc = -1, saw_status = 0, is_2xx = 0, is_expected = 0;
    if (connect(fd, (struct sockaddr *)&CFG.addr, sizeof(CFG.addr)) != 0) {
        atomic_fetch_add(&CTR.connect_fail, 1);
        goto done;
    }
    atomic_store(&CTR.last_progress_ns, now_ns()); /* board accepted: it is alive */
    size_t off = 0;
    while (off < CFG.req_len) {
        ssize_t w = send(fd, CFG.req + off, CFG.req_len - off, MSG_NOSIGNAL);
        if (w <= 0) {
            atomic_fetch_add(&CTR.io_error, 1);
            goto done;
        }
        off += (size_t)w;
    }
    uint8_t buf[2048];
    size_t total = 0;
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r > 0) {
            atomic_store(&CTR.last_progress_ns, now_ns());
            if (!saw_status && total == 0 && r >= 12 && !memcmp(buf, "HTTP/1.1 ", 9)) {
                int s = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
                saw_status = 1;
                is_2xx = (s >= 200 && s < 300);
                is_expected = (CFG.expect_status == 0 || s == CFG.expect_status);
            }
            total += (size_t)r;
            continue;
        }
        if (r == 0) {        /* board closed: clean end, no host TIME_WAIT */
            atomic_store(&CTR.last_progress_ns, now_ns());
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            atomic_fetch_add(&CTR.timeout, 1);
            goto done;
        }
        if (errno == ECONNRESET) { /* a reset is still a sign of life */
            atomic_store(&CTR.last_progress_ns, now_ns());
        }
        atomic_fetch_add(&CTR.io_error, 1);
        goto done;
    }
    rc = 0;
    atomic_fetch_add(&CTR.completed, 1);
    if (is_2xx && CFG.expect_status && CFG.expect_status / 100 != 2) {
        atomic_fetch_add(&CTR.status_2xx, 1); /* WRONG ALLOW on a deny flood */
    }
    if (saw_status && is_expected) {
        atomic_fetch_add(&CTR.status_expected, 1);
    } else if (saw_status) {
        atomic_fetch_add(&CTR.status_other, 1);
    }
done:
    atomic_fetch_sub(&g_outstanding, 1);
    close(fd);
    return rc;
}

/* Worker: send its share of the rate on a fixed monotonic schedule. If it
 * falls behind (the board/host cannot keep up) it drops the backlog rather
 * than bursting, so the offered rate stays an upper bound, never exceeded. */
struct worker_arg { int id; double rate; };

static void *worker(void *p)
{
    struct worker_arg *a = p;
    const uint64_t period_ns = (uint64_t)(1e9 / a->rate);
    uint64_t next = CFG.t0;
    while (!g_stop) {
        uint64_t t = now_ns();
        if (t < next) {
            uint64_t d = next - t;
            struct timespec ts = { d / 1000000000ull, d % 1000000000ull };
            nanosleep(&ts, NULL);
        }
        if (g_stop) {
            break;
        }
        do_request();
        next += period_ns;
        if (now_ns() > next + 200000000ull) {
            next = now_ns(); /* > 200 ms behind: resync, do not burst */
        }
    }
    return NULL;
}

static struct legit_sample legit_log[MAX_SECONDS * 10];
static _Atomic int legit_n;

/* Legitimate client: one admin request every 100 ms, status + latency. */
static void *legit_thread(void *unused)
{
    (void)unused;
    extern int legit_request(double *ms); /* defined below */
    uint64_t next = CFG.t0;
    while (!g_stop) {
        uint64_t t = now_ns();
        if (t < next) {
            uint64_t d = next - t;
            struct timespec ts = { d / 1000000000ull, d % 1000000000ull };
            nanosleep(&ts, NULL);
        }
        if (g_stop) {
            break;
        }
        double ms = 0;
        int s = legit_request(&ms);
        int i = atomic_fetch_add(&legit_n, 1);
        if (i < (int)(sizeof(legit_log) / sizeof(legit_log[0]))) {
            legit_log[i].status = s;
            legit_log[i].latency_ms = ms;
        }
        next += 100000000ull;
    }
    return NULL;
}

/* The legitimate request uses a second config (allowed path + admin token),
 * connecting to the same board address. It reads to EOF like do_request. */
static const uint8_t *LEGIT_REQ;
static size_t LEGIT_LEN;
static int LEGIT_EXPECT;

int legit_request(double *ms)
{
    uint64_t start = now_ns();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int status = -1;
    if (connect(fd, (struct sockaddr *)&CFG.addr, sizeof(CFG.addr)) != 0) {
        close(fd);
        *ms = (now_ns() - start) / 1e6;
        return -2;
    }
    if (send(fd, LEGIT_REQ, LEGIT_LEN, MSG_NOSIGNAL) != (ssize_t)LEGIT_LEN) {
        close(fd);
        *ms = (now_ns() - start) / 1e6;
        return -3;
    }
    uint8_t buf[2048];
    size_t total = 0;
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r > 0) {
            if (total == 0 && r >= 12 && !memcmp(buf, "HTTP/1.1 ", 9)) {
                status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
            }
            total += (size_t)r;
            continue;
        }
        break;
    }
    close(fd);
    *ms = (now_ns() - start) / 1e6;
    return status;
}

/* Watchdog: if nothing came back from the board for dead_ms while requests
 * were outstanding, stop the whole run. A board that stopped answering must
 * not keep being contacted (its MAC would age out of the switch). */
static int DEAD_MS = 1000;

static void *deadman(void *unused)
{
    (void)unused;
    atomic_store(&CTR.last_progress_ns, now_ns());
    while (!g_stop) {
        struct timespec ts = { 0, 100000000ull };
        nanosleep(&ts, NULL);
        uint64_t last = atomic_load(&CTR.last_progress_ns);
        if (atomic_load(&g_outstanding) > 0 &&
            now_ns() - last > (uint64_t)DEAD_MS * 1000000ull) {
            fprintf(stderr, "deadman: no response from board for %d ms, stopping\n", DEAD_MS);
            g_stop = 1;
            return NULL;
        }
    }
    return NULL;
}

static size_t read_file(const char *path, uint8_t **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        exit(2);
    }
    fclose(f);
    b[n] = 0;
    *out = b;
    return (size_t)n;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s --host IP --port N --req FILE --expect STATUS --rate R --dur S\n"
        "          --legit-req FILE --legit-expect STATUS [--threads N] [--conn-timeout-ms M]\n"
        "          [--max-mbps X] [--dead-ms M] [--direct-link]\n"
        "Offered rate above 1000 req/s requires --direct-link.\n", p);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *host = NULL, *reqf = NULL, *legitf = NULL;
    int port = 80;
    double max_mbps = 50.0;
    int direct = 0;
    CFG.threads = 16;
    CFG.conn_timeout_ms = 2000;
    CFG.expect_status = 0;
    LEGIT_EXPECT = 200;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host")) host = argv[++i];
        else if (!strcmp(argv[i], "--port")) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--req")) reqf = argv[++i];
        else if (!strcmp(argv[i], "--expect")) CFG.expect_status = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate")) CFG.rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dur")) CFG.duration_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads")) CFG.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--conn-timeout-ms")) CFG.conn_timeout_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--legit-req")) legitf = argv[++i];
        else if (!strcmp(argv[i], "--legit-expect")) LEGIT_EXPECT = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-mbps")) max_mbps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dead-ms")) DEAD_MS = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--direct-link")) direct = 1;
        else usage(argv[0]);
    }
    if (!host || !reqf || CFG.rate <= 0 || CFG.duration_s <= 0) usage(argv[0]);
    if (CFG.duration_s > MAX_SECONDS) { fprintf(stderr, "duration too long\n"); return 2; }

    /* Safety gate 1: rate ceiling without a dedicated link. */
    if (CFG.rate > 1000.0 && !direct) {
        fprintf(stderr, "refusing %.0f req/s over a shared link: pass --direct-link "
                        "only on a point-to-point cable to the board\n", CFG.rate);
        return 3;
    }

    CFG.req_len = read_file(reqf, (uint8_t **)&CFG.req);
    if (legitf) { LEGIT_LEN = read_file(legitf, (uint8_t **)&LEGIT_REQ); }

    /* Safety gate 2: estimated wire bandwidth (request + a ~256 B reply + ~
     * 400 B of TCP handshake/teardown overhead per connection). */
    double bytes_per_req = (double)CFG.req_len + 256.0 + 400.0;
    double mbps = CFG.rate * bytes_per_req * 8.0 / 1e6;
    if (mbps > max_mbps) {
        fprintf(stderr, "refusing: estimated %.1f Mbps > --max-mbps %.1f\n", mbps, max_mbps);
        return 3;
    }

    memset(&CFG.addr, 0, sizeof(CFG.addr));
    CFG.addr.sin_family = AF_INET;
    CFG.addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &CFG.addr.sin_addr) != 1) {
        fprintf(stderr, "bad host %s\n", host);
        return 2;
    }

    fprintf(stderr, "loadgen: %s:%d rate=%.0f/s threads=%d dur=%ds expect=%d "
                    "est=%.1fMbps direct=%d dead=%dms\n",
            host, port, CFG.rate, CFG.threads, CFG.duration_s, CFG.expect_status,
            mbps, direct, DEAD_MS);

    CFG.t0 = now_ns();
    pthread_t dm, lt;
    pthread_create(&dm, NULL, deadman, NULL);
    if (legitf) pthread_create(&lt, NULL, legit_thread, NULL);
    pthread_t *w = calloc(CFG.threads, sizeof(*w));
    struct worker_arg *wa = calloc(CFG.threads, sizeof(*wa));
    for (int i = 0; i < CFG.threads; i++) {
        wa[i].id = i;
        wa[i].rate = CFG.rate / CFG.threads;
        pthread_create(&w[i], NULL, worker, &wa[i]);
    }

    /* Per-second timeline: snapshot the completed counter each second. */
    uint64_t prev = 0;
    printf("{\"timeline\":[");
    int first = 1;
    for (int s = 0; s < CFG.duration_s && !g_stop; s++) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        uint64_t c = atomic_load(&CTR.completed);
        printf("%s{\"t\":%d,\"served_per_s\":%llu,\"expected\":%llu,\"wrong_allow\":%llu}",
               first ? "" : ",", s, (unsigned long long)(c - prev),
               (unsigned long long)atomic_load(&CTR.status_expected),
               (unsigned long long)atomic_load(&CTR.status_2xx));
        first = 0;
        prev = c;
        fflush(stdout);
    }
    g_stop = 1;
    for (int i = 0; i < CFG.threads; i++) pthread_join(w[i], NULL);
    pthread_join(dm, NULL);
    if (legitf) pthread_join(lt, NULL);

    int ln = atomic_load(&legit_n);
    if (ln > (int)(sizeof(legit_log) / sizeof(legit_log[0]))) ln = sizeof(legit_log) / sizeof(legit_log[0]);
    printf("],\"legit\":[");
    for (int i = 0; i < ln; i++) {
        printf("%s[%d,%.3f]", i ? "" : "", legit_log[i].status, legit_log[i].latency_ms);
        if (i + 1 < ln) printf(",");
    }
    printf("],\"totals\":{\"attempted\":%llu,\"completed\":%llu,\"connect_fail\":%llu,"
           "\"io_error\":%llu,\"timeout\":%llu,\"status_expected\":%llu,\"status_other\":%llu,"
           "\"wrong_allow_2xx\":%llu,\"stopped_early\":%s}}\n",
           (unsigned long long)atomic_load(&CTR.attempted),
           (unsigned long long)atomic_load(&CTR.completed),
           (unsigned long long)atomic_load(&CTR.connect_fail),
           (unsigned long long)atomic_load(&CTR.io_error),
           (unsigned long long)atomic_load(&CTR.timeout),
           (unsigned long long)atomic_load(&CTR.status_expected),
           (unsigned long long)atomic_load(&CTR.status_other),
           (unsigned long long)atomic_load(&CTR.status_2xx),
           g_stop && prev == 0 ? "true" : "false");
    return 0;
}
