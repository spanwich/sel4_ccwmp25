/*
 * K1 only: lwIP (NO_SYS, raw API) on ETH1 plus a minimal HTTP/1.1 front end.
 *   GET /        -> 200, build ID
 *   GET /health  -> 200, JSON counters (serial-free check, brief V6)
 * Static IP 192.168.40.60/24, MAC 02:cc:25:00:00:01 (locally administered).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>
#include <stdio.h>

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
static uint64_t (*pongs_fn)(void);

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

/* ---- HTTP ---- */

static void http_reply(struct tcp_pcb *pcb, const char *status, const char *ctype,
                       const char *body)
{
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Connection: close\r\n\r\n", status, ctype,
                     (unsigned)strlen(body));
    tcp_write(pcb, hdr, (u16_t)n, TCP_WRITE_FLAG_COPY);
    tcp_write(pcb, body, (u16_t)strlen(body), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    char line[96];
    char body[384];

    (void)arg;
    if (!p) {
        tcp_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    u16_t n = pbuf_copy_partial(p, line, sizeof(line) - 1, 0);
    line[n] = 0;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p); /* our own pbuf: the recv callback owns it */
    http_requests++;

    if (strncmp(line, "GET /health ", 12) == 0) {
        snprintf(body, sizeof(body),
                 "{\"build\":\"%s\",\"uptime_s\":%llu,\"rx\":%llu,\"tx\":%llu,"
                 "\"rx_err\":%llu,\"tx_err\":%llu,\"dma_err\":%llu,"
                 "\"http\":%llu,\"pongs\":%llu}\n",
                 build_id,
                 (unsigned long long)((rs_cntpct() - t0) / freq),
                 (unsigned long long)eth1_stats.rx, (unsigned long long)eth1_stats.tx,
                 (unsigned long long)eth1_stats.rx_err,
                 (unsigned long long)eth1_stats.tx_err,
                 (unsigned long long)eth1_stats.dma_err,
                 (unsigned long long)http_requests,
                 (unsigned long long)(pongs_fn ? pongs_fn() : 0));
        http_reply(pcb, "200 OK", "application/json", body);
    } else if (strncmp(line, "GET / ", 6) == 0) {
        snprintf(body, sizeof(body), "ccwmp25 seL4 multikernel K1 %s\n", build_id);
        http_reply(pcb, "200 OK", "text/plain", body);
    } else {
        http_reply(pcb, "404 Not Found", "text/plain", "not found\n");
    }
    tcp_close(pcb);
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) {
        return ERR_VAL;
    }
    tcp_recv(pcb, http_recv);
    return ERR_OK;
}

void net_init(void *dma_va, uint64_t dma_pa, const char *build, uint64_t (*pongs)(void))
{
    ip4_addr_t ip, mask, gw;

    freq = rs_cntfrq();
    t0 = rs_cntpct();
    build_id = build;
    pongs_fn = pongs;
    eth1_dma_init(dma_va, dma_pa, mac);
    lwip_init();
    IP4_ADDR(&ip, 192, 168, 40, 60);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);
    netif_add(&nif, &ip, &mask, &gw, NULL, nif_init, ethernet_input);
    netif_set_default(&nif);
    netif_set_up(&nif);

    struct tcp_pcb *l = tcp_new();
    tcp_bind(l, IP_ANY_TYPE, 80);
    l = tcp_listen(l);
    tcp_accept(l, http_accept);
    put_str("net: 192.168.40.60/24 mac 02:cc:25:00:00:01, http :80\n");
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
