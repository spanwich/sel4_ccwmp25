/*
 * K1 only: lwIP (NO_SYS, raw API) on ETH1 and a raw HTTP byte forwarder to
 * K0 (see "Raw HTTP forwarder" below), plus a control port for health.
 * Static IP 192.168.40.129/24, MAC 02:cc:25:00:00:01 (locally administered).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>

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

/* ---- Raw HTTP forwarder (K1, untrusted) ----
 *
 * Mirrors x86 demo/pds/lwip_proxy.c: one connection in flight; the request
 * is read into a DEMO_PAYLOAD_BYTES buffer until the header end and
 * Content-Length bytes (framing only, as on x86) have arrived; the bytes go
 * to K0 unchanged as DEMO_MESSAGE_RAW_HTTP and K0's HTTP response bytes are
 * written back. K1 parses no path or header beyond that framing, maps no
 * token and sets no role, scope or rate. Board differences: port 80 (x86:
 * 8080); a request that would overflow the buffer is reset (x86 refuses the
 * pbuf and the client stalls); K0 is called synchronously (bounded wait).
 *
 * Port 81 is a K1-local control port (not HTTP): one line "health", "stats"
 * or "probe <name>", answered with JSON. It never reaches K0's gate.
 */
#include "demo_wire.h"
#include "gateway_abi.h"

#define DEMO_HTTP_PORT 80u
#define CONTROL_PORT 81u
#define K0_TIMEOUT_DIV 2u /* K0 deadline: 1/2 s */
#define STATS_MAX 64u

static struct tcp_pcb *active_pcb;
static uint16_t next_request_epoch = 1u;
static uint16_t active_request_epoch;
static uint8_t request_buffer[DEMO_PAYLOAD_BYTES];
static uint32_t request_length;
static uint32_t response_unacked;
static bool request_ready;
static uint8_t response_buffer[DEMO_PAYLOAD_BYTES];
static uint32_t next_msg_id = 1;
static uint64_t k0_calls, k0_timeouts, refused, oversize;

/* Per-request measurement, read and cleared through "stats" on port 81. */
static struct {
    uint64_t k1_cycles, k0_cycles;
    uint32_t gate, auth, app;
    uint16_t status;
} stats[STATS_MAX];
static uint32_t n_stats;

static uint32_t find_header_end(const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0u; i + 3u < length; ++i) {
        if (bytes[i] == '\r' && bytes[i + 1u] == '\n' &&
            bytes[i + 2u] == '\r' && bytes[i + 3u] == '\n') {
            return i + 4u;
        }
    }
    return 0u;
}

static uint32_t parse_content_length(const uint8_t *bytes, uint32_t header_end)
{
    static const char field[] = "Content-Length:";
    const size_t field_length = sizeof(field) - 1u;
    for (uint32_t i = 0u; i + field_length < header_end; ++i) {
        if (memcmp(bytes + i, field, field_length) != 0) {
            continue;
        }
        uint32_t cursor = i + (uint32_t)field_length;
        while (cursor < header_end &&
               (bytes[cursor] == ' ' || bytes[cursor] == '\t')) {
            ++cursor;
        }
        uint32_t value = 0u;
        while (cursor < header_end && bytes[cursor] >= '0' &&
               bytes[cursor] <= '9') {
            if (value > (DEMO_PAYLOAD_BYTES - 9u) / 10u) {
                return DEMO_PAYLOAD_BYTES + 1u;
            }
            value = value * 10u + (uint32_t)(bytes[cursor] - '0');
            ++cursor;
        }
        return value;
    }
    return 0u;
}

static bool request_complete(void)
{
    const uint32_t header_end = find_header_end(request_buffer, request_length);
    if (header_end == 0u) {
        return false;
    }
    const uint32_t body_length =
        parse_content_length(request_buffer, header_end);
    return body_length <= DEMO_PAYLOAD_BYTES &&
        request_length >= header_end + body_length;
}

int k1_exchange(uint16_t kind, uint16_t epoch, const uint8_t *req, uint32_t len,
                uint8_t *rsp, uint32_t cap, uint32_t *rsp_len, gw_frag_hdr_t *meta)
{
    static uint8_t frag[MK_CHANNEL_PAYLOAD_SIZE];
    static mk_channel_message_t m;
    gw_frag_hdr_t h = { .msg_id = next_msg_id++, .total = len, .kind = kind, .epoch = epoch };
    uint32_t off = 0;

    if (len > GW_MSG_MAX) {
        return -1;
    }
    do {
        uint32_t n = len - off < GW_FRAG_DATA ? len - off : GW_FRAG_DATA;
        h.offset = off;
        memcpy(frag, &h, sizeof(h));
        memcpy(frag + sizeof(h), req + off, n);
        if (k1_send_frame(GW_MSG_REQ_FRAG, frag, (uint32_t)sizeof(h) + n) != 0) {
            return -1;
        }
        off += n;
    } while (off < len);

    const uint64_t deadline = k1_now() + k1_freq() / K0_TIMEOUT_DIV;
    uint32_t have = 0, total = 0;
    for (;;) {
        gw_frag_hdr_t r;
        if (k1_recv_frame(&m, deadline) != 0) {
            return -1;
        }
        if (m.type == GW_MSG_K0_STATS) {
            k1_on_k0_stats(m.payload, m.length);
            continue;
        }
        if (m.type != GW_MSG_RSP_FRAG || m.length < sizeof(r)) {
            continue; /* not ours (e.g. a late reply) */
        }
        memcpy(&r, m.payload, sizeof(r));
        uint32_t n = m.length - (uint32_t)sizeof(r);
        if (r.msg_id != h.msg_id) {
            continue;
        }
        if (r.offset == 0) {
            total = r.total;
            have = 0;
        }
        if (r.offset != have || total > cap || n > total - have) {
            return -1;
        }
        memcpy(rsp + have, m.payload + sizeof(r), n);
        have += n;
        if (have == total) {
            if (meta) {
                *meta = r;
            }
            *rsp_len = total;
            return 0;
        }
    }
}

static void reset_connection_state(void)
{
    active_pcb = NULL;
    active_request_epoch = 0u;
    request_length = 0u;
    response_unacked = 0u;
    request_ready = false;
}

static err_t response_sent(void *argument, struct tcp_pcb *pcb, u16_t length)
{
    (void)argument;
    if (pcb != active_pcb) {
        return ERR_OK;
    }
    if (length >= response_unacked) {
        response_unacked = 0u;
        tcp_arg(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            reset_connection_state();
            return ERR_ABRT;
        }
        reset_connection_state();
    } else {
        response_unacked -= length;
    }
    return ERR_OK;
}

static void connection_error(void *argument, err_t error)
{
    struct tcp_pcb *failed_pcb = argument;
    (void)error;
    if (failed_pcb == active_pcb) {
        reset_connection_state();
    }
}

/* Forward the buffered request to K0 and queue K0's response bytes.
 * Returns ERR_ABRT if the connection was aborted. */
static err_t forward_request(struct tcp_pcb *pcb)
{
    gw_frag_hdr_t meta;
    uint32_t n = 0;
    uint64_t c0 = rs_pmccntr();
    int rc = k1_exchange(DEMO_MESSAGE_RAW_HTTP, active_request_epoch, request_buffer,
                         request_length, response_buffer, sizeof(response_buffer), &n, &meta);
    uint64_t k1_cycles = rs_pmccntr() - c0;
    k0_calls++;
    if (rc != 0 || meta.kind != DEMO_MESSAGE_HTTP_RESPONSE || n == 0 || n > UINT16_MAX ||
        meta.epoch != active_request_epoch) {
        k0_timeouts++;
        tcp_abort(pcb); /* x86 lwip_proxy drops a request with no valid response */
        reset_connection_state();
        return ERR_ABRT;
    }
    if (n_stats < STATS_MAX) {
        uint16_t status = 0;
        if (n >= 12) {
            status = (uint16_t)((response_buffer[9] - '0') * 100 + (response_buffer[10] - '0') * 10 +
                                (response_buffer[11] - '0'));
        }
        stats[n_stats].k1_cycles = k1_cycles;
        stats[n_stats].k0_cycles = meta.k0_cycles;
        stats[n_stats].gate = meta.gate_cycles;
        stats[n_stats].auth = meta.auth_cycles;
        stats[n_stats].app = meta.app_cycles;
        stats[n_stats].status = status;
        n_stats++;
    }
    if (tcp_write(pcb, response_buffer, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        tcp_abort(pcb);
        reset_connection_state();
        return ERR_ABRT;
    }
    response_unacked = n;
    tcp_output(pcb);
    return ERR_OK;
}

static err_t receive_http(void *argument, struct tcp_pcb *pcb, struct pbuf *pbuf, err_t error)
{
    (void)argument;
    if (pcb != active_pcb) {
        if (pbuf != NULL) {
            pbuf_free(pbuf);
        }
        return ERR_OK;
    }
    if (pbuf == NULL) {
        tcp_close(pcb);
        reset_connection_state();
        return ERR_OK;
    }
    if (error != ERR_OK) {
        pbuf_free(pbuf);
        return error;
    }
    if (request_ready) {
        tcp_recved(pcb, pbuf->tot_len);
        pbuf_free(pbuf); /* bytes after a complete request are ignored */
        return ERR_OK;
    }
    if (request_length + pbuf->tot_len > sizeof(request_buffer)) {
        oversize++;
        pbuf_free(pbuf);
        tcp_abort(pcb);
        reset_connection_state();
        return ERR_ABRT;
    }
    const u16_t copied = pbuf_copy_partial(pbuf, request_buffer + request_length,
                                           pbuf->tot_len, 0u);
    request_length += copied;
    tcp_recved(pcb, copied);
    pbuf_free(pbuf);

    if (request_complete()) {
        request_ready = true;
        http_requests++;
        return forward_request(pcb);
    }
    return ERR_OK;
}

static err_t accept_http(void *argument, struct tcp_pcb *pcb, err_t error)
{
    (void)argument;
    if (error != ERR_OK || active_pcb != NULL) {
        refused++;
        return ERR_MEM; /* lwIP aborts it: one request in flight, as on x86 */
    }
    reset_connection_state();
    active_pcb = pcb;
    active_request_epoch = next_request_epoch;
    ++next_request_epoch;
    if (next_request_epoch == 0u) {
        next_request_epoch = 1u;
    }
    tcp_nagle_disable(pcb);
    tcp_arg(pcb, pcb);
    tcp_recv(pcb, receive_http);
    tcp_sent(pcb, response_sent);
    tcp_err(pcb, connection_error);
    return ERR_OK;
}

/* ---- Port 81: K1-local control (health, per-request stats, D7 probes) ---- */

static char ctl_line[64];
static uint32_t ctl_len;
static char ctl_out[STATS_MAX * 112u + 512u];

static int health(char *out, unsigned cap)
{
    return snprintf(out, cap,
             "{\"build\":\"%s\",\"uptime_s\":%llu,\"rx\":%llu,\"tx\":%llu,"
             "\"rx_err\":%llu,\"tx_err\":%llu,\"dma_err\":%llu,\"http\":%llu,"
             "\"k0_calls\":%llu,\"k0_timeouts\":%llu,\"refused\":%llu,\"oversize\":%llu}\n",
             build_id, (unsigned long long)((rs_cntpct() - t0) / freq),
             (unsigned long long)eth1_stats.rx, (unsigned long long)eth1_stats.tx,
             (unsigned long long)eth1_stats.rx_err, (unsigned long long)eth1_stats.tx_err,
             (unsigned long long)eth1_stats.dma_err, (unsigned long long)http_requests,
             (unsigned long long)k0_calls, (unsigned long long)k0_timeouts,
             (unsigned long long)refused, (unsigned long long)oversize);
}

/* ---- Resilience counters (1 s), drained through "counters" on port 81 ---- */

#define CTR_MAX 256u
#define CTR_PER_READ 16u
static gw_k0_stats_t k0_ctr[CTR_MAX];
static uint32_t k0_ctr_head, k0_ctr_tail, k0_ctr_lost;
static struct {
    uint64_t tick, uptime_ms, http, refused, k0_calls, k0_timeouts, oversize, rx, tx, rx_err;
    uint32_t busy_ppm;
} k1_ctr[CTR_MAX];
static uint32_t k1_ctr_head, k1_ctr_tail, k1_ctr_lost;

void k1_on_k0_stats(const void *payload, uint32_t len)
{
    if (len != sizeof(gw_k0_stats_t)) {
        return;
    }
    if (k0_ctr_head - k0_ctr_tail == CTR_MAX) {
        k0_ctr_tail++;
        k0_ctr_lost++;
    }
    memcpy(&k0_ctr[k0_ctr_head++ % CTR_MAX], payload, len);
}

void net_tick(uint64_t tick, uint32_t busy_ppm)
{
    if (k1_ctr_head - k1_ctr_tail == CTR_MAX) {
        k1_ctr_tail++;
        k1_ctr_lost++;
    }
    uint32_t i = k1_ctr_head++ % CTR_MAX;
    k1_ctr[i].tick = tick;
    k1_ctr[i].uptime_ms = (rs_cntpct() - t0) * 1000u / freq;
    k1_ctr[i].busy_ppm = busy_ppm;
    k1_ctr[i].http = http_requests;
    k1_ctr[i].refused = refused;
    k1_ctr[i].k0_calls = k0_calls;
    k1_ctr[i].k0_timeouts = k0_timeouts;
    k1_ctr[i].oversize = oversize;
    k1_ctr[i].rx = eth1_stats.rx;
    k1_ctr[i].tx = eth1_stats.tx;
    k1_ctr[i].rx_err = eth1_stats.rx_err;
}

static int counters_json(char *out, unsigned cap)
{
    int n = snprintf(out, cap, "{\"k0_lost\":%u,\"k1_lost\":%u,\"k0\":[", k0_ctr_lost, k1_ctr_lost);
    for (uint32_t c = 0; c < CTR_PER_READ && k0_ctr_tail != k0_ctr_head && (unsigned)n < cap; c++) {
        const gw_k0_stats_t *e = &k0_ctr[k0_ctr_tail++ % CTR_MAX];
        n += snprintf(out + n, cap - (unsigned)n,
                      "%s{\"tick\":%llu,\"uptime_ms\":%llu,\"busy_ppm\":%u,\"pets\":%u,"
                      "\"served\":%llu,\"rejected_frames\":%llu,\"no_response\":%llu,"
                      "\"queue_depth\":%u,\"send_failures\":%u,\"corrupt_dropped\":%llu}",
                      c ? "," : "", (unsigned long long)e->tick, (unsigned long long)e->uptime_ms,
                      e->busy_ppm, e->pets, (unsigned long long)e->served,
                      (unsigned long long)e->rejected_frames, (unsigned long long)e->no_response,
                      e->queue_depth, e->send_failures, (unsigned long long)e->corrupt_dropped);
    }
    n += snprintf(out + n, cap - (unsigned)n, "],\"k1\":[");
    for (uint32_t c = 0; c < CTR_PER_READ && k1_ctr_tail != k1_ctr_head && (unsigned)n < cap; c++) {
        uint32_t i = k1_ctr_tail++ % CTR_MAX;
        n += snprintf(out + n, cap - (unsigned)n,
                      "%s{\"tick\":%llu,\"uptime_ms\":%llu,\"busy_ppm\":%u,\"http\":%llu,"
                      "\"refused\":%llu,\"k0_calls\":%llu,\"k0_timeouts\":%llu,\"oversize\":%llu,"
                      "\"rx\":%llu,\"tx\":%llu,\"rx_err\":%llu}",
                      c ? "," : "", (unsigned long long)k1_ctr[i].tick,
                      (unsigned long long)k1_ctr[i].uptime_ms, k1_ctr[i].busy_ppm,
                      (unsigned long long)k1_ctr[i].http, (unsigned long long)k1_ctr[i].refused,
                      (unsigned long long)k1_ctr[i].k0_calls, (unsigned long long)k1_ctr[i].k0_timeouts,
                      (unsigned long long)k1_ctr[i].oversize, (unsigned long long)k1_ctr[i].rx,
                      (unsigned long long)k1_ctr[i].tx, (unsigned long long)k1_ctr[i].rx_err);
    }
    n += snprintf(out + n, cap - (unsigned)n, "]}\n");
    return n;
}

static int stats_json(char *out, unsigned cap)
{
    int n = snprintf(out, cap, "{\"requests\":[");
    for (uint32_t i = 0; i < n_stats && n > 0 && (unsigned)n < cap; i++) {
        n += snprintf(out + n, cap - (unsigned)n,
                      "%s{\"status\":%u,\"k1\":%llu,\"k0\":%llu,\"gate\":%u,\"auth\":%u,\"app\":%u}",
                      i ? "," : "", stats[i].status, (unsigned long long)stats[i].k1_cycles,
                      (unsigned long long)stats[i].k0_cycles, stats[i].gate, stats[i].auth,
                      stats[i].app);
    }
    n_stats = 0;
    if (n > 0 && (unsigned)n < cap) {
        n += snprintf(out + n, cap - (unsigned)n, "]}\n");
    }
    return n;
}

static err_t control_recv(void *argument, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)argument;
    if (!p) {
        tcp_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    tcp_recved(pcb, p->tot_len);
    uint32_t room = (uint32_t)sizeof(ctl_line) - 1u - ctl_len;
    uint32_t take = p->tot_len < room ? p->tot_len : room;
    pbuf_copy_partial(p, ctl_line + ctl_len, (u16_t)take, 0);
    ctl_len += take;
    ctl_line[ctl_len] = 0;
    pbuf_free(p);
    char *nl = strchr(ctl_line, '\n');
    if (!nl && ctl_len < sizeof(ctl_line) - 1u) {
        return ERR_OK;
    }
    if (nl) {
        *nl = 0;
        if (nl > ctl_line && nl[-1] == '\r') {
            nl[-1] = 0;
        }
    }
    int n;
    if (strcmp(ctl_line, "health") == 0) {
        n = health(ctl_out, sizeof(ctl_out));
    } else if (strcmp(ctl_line, "stats") == 0) {
        n = stats_json(ctl_out, sizeof(ctl_out));
    } else if (strcmp(ctl_line, "counters") == 0) {
        n = counters_json(ctl_out, sizeof(ctl_out));
    } else if (strncmp(ctl_line, "probe ", 6) == 0) {
        n = k1_probe(ctl_line + 6, ctl_out, sizeof(ctl_out));
        if (n < 0) {
            n = snprintf(ctl_out, sizeof(ctl_out), "{\"error\":\"unknown probe\"}\n");
        }
    } else {
        n = snprintf(ctl_out, sizeof(ctl_out), "{\"error\":\"unknown command\"}\n");
    }
    ctl_len = 0;
    if (n > (int)sizeof(ctl_out)) {
        n = (int)sizeof(ctl_out);
    }
    for (int off = 0; off < n;) {
        int chunk = n - off < 1024 ? n - off : 1024;
        if (tcp_write(pcb, ctl_out + off, (u16_t)chunk, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }
        off += chunk;
    }
    tcp_output(pcb);
    tcp_recv(pcb, NULL);
    tcp_close(pcb);
    return ERR_OK;
}

static err_t control_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) {
        return ERR_VAL;
    }
    ctl_len = 0;
    tcp_recv(pcb, control_recv);
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
    tcp_bind(l, IP_ANY_TYPE, DEMO_HTTP_PORT);
    l = tcp_listen(l);
    tcp_accept(l, accept_http);
    struct tcp_pcb *c = tcp_new();
    tcp_bind(c, IP_ANY_TYPE, CONTROL_PORT);
    c = tcp_listen(c);
    tcp_accept(c, control_accept);
    put_str("net: 192.168.40.129/24 mac 02:cc:25:00:00:01, raw http :80 -> K0, control :81\n");
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
