/*
 * K1 only: lwIP (NO_SYS, raw API) on ETH1 plus a minimal HTTP/1.1 front end.
 *   GET /        -> 200, build ID
 *   GET /health  -> 200, JSON counters (serial-free check, brief V6)
 * HTTP model: see the "HTTP front end" comment below.
 * Static IP 192.168.40.129/24, MAC 02:cc:25:00:00:01 (locally administered).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "rs.h"
#include "dwmac.h"
#include "net.h"

#if KERNEL_ID == 1

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

static const uint8_t mac[6] = { 0x02, 0xcc, 0x25, 0x00, 0x00, 0x01 };
static struct netif nif;
static uint64_t t0, freq;
static uint64_t http_requests;
static const char *build_id = "?";

void rs_lwip_diag(const char *s)
{
    put_str("lwip assert: ");
    put_str(s);
    put_str("\n");
}

u32_t sys_now(void)
{
    return (u32_t)((rs_cntpct() - t0) * 1000u / freq);
}

unsigned int lwip_rand(void)
{
    static uint64_t x = 0x9e3779b97f4a7c15ull;
    x ^= rs_cntpct();
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (unsigned int)x;
}

static err_t link_output(struct netif *n, struct pbuf *p)
{
    static uint8_t frame[1536];
    (void)n;
    if (p->tot_len > sizeof(frame)) {
        return ERR_BUF;
    }
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    return eth1_tx(frame, p->tot_len) == 0 ? ERR_OK : ERR_MEM;
}

static err_t nif_init(struct netif *n)
{
    n->name[0] = 'e';
    n->name[1] = '1';
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, mac, 6);
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = link_output;
    return ERR_OK;
}

static void rx_frame(const uint8_t *frame, unsigned len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) {
        eth1_stats.rx_err++;
        return;
    }
    pbuf_take(p, frame, (u16_t)len);
    if (nif.input(p, &nif) != ERR_OK) {
        pbuf_free(p);
    }
}

/* ---- HTTP front end (K1) ----
 *
 * Board request model (mirrors the x86 D1 scenarios without TLS/login):
 *   Authorization: Bearer admin    -> role ADMIN,    scope 0xffff
 *   Authorization: Bearer operator -> role OPERATOR, scope 0x0001
 *   (none / unknown)               -> role NONE,     scope 0
 *   X-Rate: <n>                     -> rate_count (default 0; the board has
 *                                      no rate-counter component)
 *   /api/{login,logout,status,policy,demo,reports} -> PATH_* IDs; other
 *   /api/... paths -> FNV-1a hash (absent from any policy).
 * K1-local, not gated: GET /, GET /health, GET /probe/<name> (C6).
 */
#include "gw_proto.h"
#include "gateway_abi.h"

#define CONN_BUF 1024u
#define MAX_CONN 8u

struct conn {
    struct tcp_pcb *pcb;
    uint32_t len;
    char buf[CONN_BUF];
};
static struct conn conns[MAX_CONN];
static uint32_t next_req_id = 1;
static uint64_t k0_calls, k0_timeouts, decisions_allow, decisions_deny;

static void http_reply(struct tcp_pcb *pcb, const char *status, const char *ctype,
                       const char *extra_hdrs, const char *body, unsigned body_len)
{
    char hdr[320];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "%sConnection: close\r\n\r\n", status, ctype, body_len,
                     extra_hdrs ? extra_hdrs : "");
    tcp_write(pcb, hdr, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (body_len) {
        tcp_write(pcb, body, (u16_t)body_len, TCP_WRITE_FLAG_COPY);
    }
    tcp_output(pcb);
}

static void reply_text(struct tcp_pcb *pcb, const char *status, const char *body)
{
    http_reply(pcb, status, "text/plain", NULL, body, (unsigned)strlen(body));
}

static const char *find_header(const char *req, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && p[2] != '\r'; p = strstr(p + 2, "\r\n")) {
        if (strncasecmp(p + 2, name, n) == 0 && p[2 + n] == ':') {
            const char *v = p + 3 + n;
            while (*v == ' ') {
                v++;
            }
            return v;
        }
    }
    return NULL;
}

static uint32_t path_id_of(const char *path, size_t n)
{
    static const struct { const char *p; uint32_t id; } map[] = {
        { "/api/login", PATH_LOGIN }, { "/api/logout", PATH_LOGOUT },
        { "/api/status", PATH_STATUS }, { "/api/policy", PATH_POLICY },
        { "/api/demo", PATH_DEMO }, { "/api/reports", PATH_REPORTS },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strlen(map[i].p) == n && strncmp(path, map[i].p, n) == 0) {
            return map[i].id;
        }
    }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ (uint8_t)path[i]) * 16777619u;
    }
    return h == INACTIVE_PATH_ID ? h + 1 : h;
}

static uint8_t method_of(const char *m, size_t n)
{
    static const struct { const char *s; uint8_t v; } map[] = {
        { "GET", METHOD_GET }, { "POST", METHOD_POST }, { "PUT", METHOD_PUT },
        { "DELETE", METHOD_DELETE }, { "PATCH", METHOD_PATCH }, { "HEAD", METHOD_HEAD },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strlen(map[i].s) == n && strncmp(m, map[i].s, n) == 0) {
            return map[i].v;
        }
    }
    return 0;
}

static void health(struct tcp_pcb *pcb)
{
    char body[512];
    int n = snprintf(body, sizeof(body),
             "{\"build\":\"%s\",\"uptime_s\":%llu,\"rx\":%llu,\"tx\":%llu,"
             "\"rx_err\":%llu,\"tx_err\":%llu,\"dma_err\":%llu,\"http\":%llu,"
             "\"k0_calls\":%llu,\"k0_timeouts\":%llu,\"allow\":%llu,\"deny\":%llu}\n",
             build_id, (unsigned long long)((rs_cntpct() - t0) / freq),
             (unsigned long long)eth1_stats.rx, (unsigned long long)eth1_stats.tx,
             (unsigned long long)eth1_stats.rx_err, (unsigned long long)eth1_stats.tx_err,
             (unsigned long long)eth1_stats.dma_err, (unsigned long long)http_requests,
             (unsigned long long)k0_calls, (unsigned long long)k0_timeouts,
             (unsigned long long)decisions_allow, (unsigned long long)decisions_deny);
    http_reply(pcb, "200 OK", "application/json", NULL, body, (unsigned)n);
}

/* Forward one decision/update to K0 and reply to the client. */
static void gated(struct tcp_pcb *pcb, gw_req_t *req, uint16_t type)
{
    static gw_rsp_t rsp;
    char hdrs[160];

    req->req_id = next_req_id++;
    uint64_t c0 = rs_pmccntr();
    int rc = k1_call_k0(type, req, (uint32_t)(offsetof(gw_req_t, body) + req->body_len),
                        &rsp, sizeof(rsp));
    uint64_t k1_cycles = rs_pmccntr() - c0;
    k0_calls++;
    if (rc != 0 || rsp.req_id != req->req_id) {
        k0_timeouts++;
        reply_text(pcb, "503 Service Unavailable", "k0 did not answer\n");
        return;
    }
    snprintf(hdrs, sizeof(hdrs),
             "X-Engine-Cycles: %llu\r\nX-Channel-Cycles: %llu\r\nX-Policy-Digest: %016llx\r\n",
             (unsigned long long)rsp.engine_cycles, (unsigned long long)k1_cycles,
             (unsigned long long)rsp.policy_digest);
    if (type == GW_MSG_UPDATE) {
        if (rsp.result == UPDATE_APPLIED) {
            http_reply(pcb, "200 OK", "text/plain", hdrs, "applied\n", 8);
        } else {
            http_reply(pcb, "403 Forbidden", "text/plain", hdrs, "rejected\n", 9);
        }
        return;
    }
    switch (rsp.result) {
    case DECISION_ALLOW:
        decisions_allow++;
        http_reply(pcb, "200 OK", "application/json", hdrs, rsp.body,
                   rsp.body_len <= sizeof(rsp.body) ? rsp.body_len : 0);
        break;
    case DECISION_DENY:
        decisions_deny++;
        http_reply(pcb, "403 Forbidden", "text/plain", hdrs, "denied\n", 7);
        break;
    case DECISION_RATE_LIMITED:
        decisions_deny++;
        http_reply(pcb, "429 Too Many Requests", "text/plain", hdrs, "rate limited\n", 13);
        break;
    default:
        http_reply(pcb, "500 Internal Server Error", "text/plain", hdrs, "error\n", 6);
        break;
    }
}

static void handle_request(struct tcp_pcb *pcb, char *req, uint32_t body_off, uint32_t body_len)
{
    static gw_req_t g;
    http_requests++;

    const char *sp1 = strchr(req, ' ');
    const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) {
        reply_text(pcb, "400 Bad Request", "bad request line\n");
        return;
    }
    const char *path = sp1 + 1;
    size_t plen = (size_t)(sp2 - path);
    if (plen == 1 && *path == '/') {
        char body[96];
        int n = snprintf(body, sizeof(body), "ccwmp25 seL4 multikernel K1 %s\n", build_id);
        http_reply(pcb, "200 OK", "text/plain", NULL, body, (unsigned)n);
        return;
    }
    if (plen == 7 && strncmp(path, "/health", 7) == 0) {
        health(pcb);
        return;
    }
    if (plen > 7 && strncmp(path, "/probe/", 7) == 0) {
        char out[384];
        char name[32];
        size_t nl = plen - 7 < sizeof(name) - 1 ? plen - 7 : sizeof(name) - 1;
        memcpy(name, path + 7, nl);
        name[nl] = 0;
        int n = k1_probe(name, out, sizeof(out));
        http_reply(pcb, n >= 0 ? "200 OK" : "404 Not Found", "application/json", NULL,
                   out, n >= 0 ? (unsigned)n : 0);
        return;
    }
    if (plen < 5 || strncmp(path, "/api/", 5) != 0) {
        reply_text(pcb, "404 Not Found", "not found\n");
        return;
    }

    memset(&g, 0, sizeof(g));
    g.method = method_of(req, (size_t)(sp1 - req));
    g.path_id = path_id_of(path, plen);
    const char *auth = find_header(req, "Authorization");
    if (auth && strncmp(auth, "Bearer admin\r", 13) == 0) {
        g.role = ROLE_ADMIN;
        g.scope = 0xffff;
    } else if (auth && strncmp(auth, "Bearer operator\r", 16) == 0) {
        g.role = ROLE_OPERATOR;
        g.scope = 0x0001;
    }
    const char *rate = find_header(req, "X-Rate");
    if (rate) {
        unsigned long r = strtoul(rate, NULL, 10);
        g.rate_count = (uint8_t)(r > 255 ? 255 : r);
    }
    if (g.path_id == PATH_POLICY && g.method == METHOD_PUT) {
        g.body_len = body_len > GW_BODY_MAX ? GW_BODY_MAX : body_len;
        memcpy(g.body, req + body_off, g.body_len);
        gated(pcb, &g, GW_MSG_UPDATE);
        return;
    }
    gated(pcb, &g, GW_MSG_DECIDE);
}

static struct conn *conn_of(struct tcp_pcb *pcb)
{
    for (unsigned i = 0; i < MAX_CONN; i++) {
        if (conns[i].pcb == pcb) {
            return &conns[i];
        }
    }
    for (unsigned i = 0; i < MAX_CONN; i++) {
        if (!conns[i].pcb) {
            conns[i].pcb = pcb;
            conns[i].len = 0;
            return &conns[i];
        }
    }
    return NULL;
}

static void conn_release(struct tcp_pcb *pcb)
{
    for (unsigned i = 0; i < MAX_CONN; i++) {
        if (conns[i].pcb == pcb) {
            conns[i].pcb = NULL;
        }
    }
}

static void http_err(void *arg, err_t err)
{
    (void)err;
    conn_release((struct tcp_pcb *)arg);
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (!p) {
        conn_release(pcb);
        tcp_arg(pcb, NULL);
        tcp_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    struct conn *c = conn_of(pcb);
    tcp_recved(pcb, p->tot_len);
    if (!c || c->len + p->tot_len >= CONN_BUF) {
        pbuf_free(p); /* upstream lwIP 2.1: the callee frees on ERR_OK */
        reply_text(pcb, "413 Payload Too Large", "too large\n");
        conn_release(pcb);
        tcp_close(pcb);
        return ERR_OK;
    }
    pbuf_copy_partial(p, c->buf + c->len, p->tot_len, 0);
    c->len += p->tot_len;
    c->buf[c->len] = 0;
    pbuf_free(p);

    char *end = strstr(c->buf, "\r\n\r\n");
    if (!end) {
        return ERR_OK; /* headers incomplete */
    }
    uint32_t body_off = (uint32_t)(end + 4 - c->buf);
    const char *cl = find_header(c->buf, "Content-Length");
    uint32_t want = cl ? (uint32_t)strtoul(cl, NULL, 10) : 0;
    if (c->len - body_off < want) {
        return ERR_OK; /* body incomplete */
    }
    handle_request(pcb, c->buf, body_off, want);
    conn_release(pcb);
    tcp_arg(pcb, NULL);
    tcp_close(pcb);
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) {
        return ERR_VAL;
    }
    tcp_arg(pcb, pcb);
    tcp_err(pcb, http_err);
    tcp_recv(pcb, http_recv);
    tcp_nagle_disable(pcb);
    return ERR_OK;
}

void net_init(void *dma_va, uint64_t dma_pa, const char *build)
{
    ip4_addr_t ip, mask, gw;

    freq = rs_cntfrq();
    t0 = rs_cntpct();
    build_id = build;
    eth1_dma_init(dma_va, dma_pa, mac);
    lwip_init();
    IP4_ADDR(&ip, 192, 168, 40, 129);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);
    netif_add(&nif, &ip, &mask, &gw, NULL, nif_init, ethernet_input);
    netif_set_default(&nif);
    netif_set_up(&nif);

    struct tcp_pcb *l = tcp_new();
    tcp_bind(l, IP_ANY_TYPE, 80);
    l = tcp_listen(l);
    tcp_accept(l, http_accept);
    put_str("net: 192.168.40.129/24 mac 02:cc:25:00:00:01, http :80\n");
}

static int link_state;

/* Poll link, deliver received frames, run lwIP timers. */
void net_poll(void)
{
    unsigned speed = 0, full = 0;
    int up = eth1_link(&speed, &full);
    if (up && !link_state) {
        eth1_mac_link(speed, full);
        netif_set_link_up(&nif);
        put_str("phy: link up ");
        put_dec(speed);
        put_str(full ? "/full\n" : "/half\n");
    } else if (!up && link_state) {
        netif_set_link_down(&nif);
        put_str("phy: link down\n");
    }
    link_state = up;
    eth1_rx(rx_frame);
    sys_check_timeouts();
}

#endif /* KERNEL_ID == 1 */
