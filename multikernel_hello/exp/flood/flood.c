/*
 * Open-loop HTTP flood generator for the CCMP25 resilience test
 * (studies/ccmp25-board/RESILIENCE_TEST.md).
 *
 * - Fixed send schedule per thread (rate / threads), independent of replies.
 * - One TCP connection per request; the request bytes come from a file.
 * - The client reads to EOF so the board closes first (no host TIME_WAIT);
 *   the reply's status line is checked against --expect.
 * - A separate "legitimate" thread sends --legit every 100 ms and records
 *   status and latency.
 * - Safety: refuses > 1000 req/s unless --direct-link; refuses a schedule
 *   whose estimated wire rate exceeds --max-mbps; stops the whole run if
 *   nothing at all (bytes, RST, connect) came back from the board for
 *   --dead-ms (default 1000) while requests were outstanding.
 *
 * Output: one JSON document (per-second timeline + totals) on stdout.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_SECONDS 4000
#define MAX_FDS 65536
#define