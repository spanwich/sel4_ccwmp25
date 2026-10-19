/*
 * K0 side of the corrected board design (studies/ccmp25-board/
 * CORRECTED_DESIGN.md): the x86 D1 gate pipeline, compiled from the
 * unchanged paper sources, behind a K0 "proxy" that speaks gw_proto.h to K1.
 *
 *   K1 raw HTTP --frags--> proxy --DemoQueue--> policy_gate <-> authenticator
 *                                                  |
 *                                         protected_app --DemoQueue--> proxy
 *                                                              --frags--> K1
 *
 * The three PDs keep their Microkit code: their shared-memory queues are
 * plain static memory here and microkit_notify() (k0shim/microkit.h) marks
 * the peer's channel pending; dispatch() delivers notified() until no
 * channel is pending, so one request runs to completion before K0 replies.
 * All PDs share K0's single address space (no VSpace isolation between the
 * gate and the authenticator on the board; recorded in the evidence).
 *
 * Images (MK_ENGINE): raw (D0a: fixed 200, no gate), gateonly (D0b: gate
 * path, accept-all engine), everparse, cedar.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "rs.h"
#include "gateway_k0.h"
#include "gw_proto.h"

#if KERNEL_ID == 0

#include "microkit.h"
#include "demo_wire.h"
#include "gateway_abi.h"
#include "gateway_common.h"
#include "gateway_engine.h"
#if MK_REAL_ENGINE
#include "gateway_benchmark_profile.h"
int equivalence_main(void);
int extractor_test_main(void);
#endif

/* The PDs' entry points and config globals, renamed per PD at compile time. */
void pg_init(void);
void pg_notified(microkit_channel ch);
void au_init(void);
void au_notified(microkit_channel ch);
void pa_init(void);
void pa_notified(microkit_channel ch);
extern DemoConfig pg_demo_config, pa_demo_config;
extern AuthConfig pg_auth_config, au_auth_config;

#define C2_WARMUP  100u
#define C2_SAMPLES 1000u

const char *gw_engine_label(void)
{
    return MK_ENGINE_NAME;
}

/* ---- Microkit stand-in: queues, channels, run-to-idle dispatcher ---- */

enum { PD_GATE, PD_AUTH, PD_APP, PD_PROXY, PD_COUNT };

static DemoQueue q_proxy_gate, q_gate_app, q_app_proxy;
static AuthQueue q_auth_req, q_auth_rsp;

/* Channel IDs, as each PD sees them (x86 meta.py wiring, same topology). */
#define CH_GATE_IN   1u /* gate <-> proxy (gate's input)   */
#define CH_GATE_OUT  2u /* gate <-> app   (gate's output)  */
#define CH_AUTH      3u /* gate <-> authenticator          */
#define CH_APP_IN    1u /* app  <-> gate  (app's input)    */
#define CH_APP_OUT   2u /* app  <-> proxy (app's output)   */
#define CH_PROXY_REQ 1u /* proxy <-> gate                  */
#define CH_PROXY_RSP 2u /* proxy <-> app                   */

static const struct {
    uint8_t pd, ch;
} route[PD_COUNT][4] = {
    [PD_GATE] = { [CH_GATE_IN] = { PD_PROXY, CH_PROXY_REQ },
                  [CH_GATE_OUT] = { PD_APP, CH_APP_IN },
                  [CH_AUTH] = { PD_AUTH, CH_AUTH } },
    [PD_AUTH] = { [CH_AUTH] = { PD_GATE, CH_AUTH } },
    [PD_APP] = { [CH_APP_IN] = { PD_GATE, CH_GATE_OUT },
                 [CH_APP_OUT] = { PD_PROXY, CH_PROXY_RSP } },
    [PD_PROXY] = { [CH_PROXY_REQ] = { PD_GATE, CH_GATE_IN },
                   [CH_PROXY_RSP] = { PD_APP, CH_APP_OUT } },
};

static unsigned current_pd = PD_PROXY;
static uint32_t pending[PD_COUNT]; /* bit per local channel */
static uint64_t pd_cycles[PD_COUNT];

void microkit_notify(microkit_channel ch)
{
    if (ch >= 4u || route[current_pd][ch].ch == 0) {
        halt("microkit_notify: unwired channel", ch);
    }
    pending[route[current_pd][ch].pd] |= 1u << route[current_pd][ch].ch;
}

void microkit_dbg_puts(const char *s)
{
    put_str(s);
}

void microkit_dbg_putc(int c)
{
    char s[2] = { (char)c, 0 };
    put_str(s);
}

void microkit_dbg_put32(uint32_t v)
{
    put_dec(v);
}

static void proxy_notified(microkit_channel ch);

static void deliver(unsigned pd, microkit_channel ch)
{
    unsigned prev = current_pd;
    current_pd = pd;
    uint64_t s = rs_pmccntr();
    switch (pd) {
    case PD_GATE: pg_notified(ch); break;
    case PD_AUTH: au_notified(ch); break;
    case PD_APP: pa_notified(ch); break;
    default: proxy_notified(ch); break;
    }
    pd_cycles[pd] += rs_pmccntr() - s;
    current_pd = prev;
}

/* Deliver pending notifications until none is left (bounded: a PD that keeps
 * re-notifying would be a bug in the port, so stop loudly). */
static void dispatch(void)
{
    for (unsigned rounds = 0; rounds < 1000u; rounds++) {
        int any = 0;
        for (unsigned pd = 0; pd < PD_COUNT; pd++) {
            while (pending[pd]) {
                unsigned ch = (unsigned)__builtin_ctz(pending[pd]);
                pending[pd] &= ~(1u << ch);
                deliver(pd, ch);
                any = 1;
            }
        }
        if (!any) {
            return;
        }
    }
    halt("dispatch: no quiescence", 0);
}

/* ---- K0 proxy: reassembly from K1, response back to K1 ---- */

static DemoMessage rx_msg;          /* request under reassembly */
static uint32_t rx_id, rx_have, rx_total;
static int rx_active;
static uint64_t frames_rejected, requests_served, queue_busy, no_response, send_failures;
static DemoMessage tx_msg;          /* response from protected_app */
static int tx_ready;

static void proxy_notified(microkit_channel ch)
{
    if (ch == CH_PROXY_RSP && demo_queue_receive(&q_app_proxy, &tx_msg)) {
        tx_ready = 1;
        microkit_notify(CH_PROXY_RSP); /* ack, as lwip_proxy.c does */
    }
}

static void send_response(uint32_t msg_id, uint16_t epoch, const uint8_t *bytes, uint32_t len,
                          uint64_t k0_cycles)
{
    uint8_t frag[512];
    gw_frag_hdr_t h = {
        .msg_id = msg_id, .total = len, .kind = DEMO_MESSAGE_HTTP_RESPONSE, .epoch = epoch,
        .k0_cycles = k0_cycles, .gate_cycles = (uint32_t)pd_cycles[PD_GATE],
        .auth_cycles = (uint32_t)pd_cycles[PD_AUTH], .app_cycles = (uint32_t)pd_cycles[PD_APP],
    };
    uint32_t off = 0;
    do {
        uint32_t n = len - off < GW_FRAG_DATA ? len - off : GW_FRAG_DATA;
        h.offset = off;
        memcpy(frag, &h, sizeof(h));
        memcpy(frag + sizeof(h), bytes + off, n);
        if (k0_send(GW_MSG_RSP_FRAG, frag, (uint32_t)sizeof(h) + n) != 0) {
            send_failures++;
            return; /* K1's ring full: K1 times out and drops the request */
        }
        off += n;
    } while (off < len);
}

static const char RAW_RESPONSE[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\n"
    "Connection: close\r\n\r\n{\"status\":\"ok\"}";

static void serve_request(uint16_t epoch)
{
    for (unsigned pd = 0; pd < PD_COUNT; pd++) {
        pd_cycles[pd] = 0;
    }
    uint64_t s = rs_pmccntr();
#if MK_ENGINE_raw
    /* D0a: network + channel cost only; the bytes are not looked at. */
    uint64_t c = rs_pmccntr() - s;
    send_response(rx_id, epoch, (const uint8_t *)RAW_RESPONSE, sizeof(RAW_RESPONSE) - 1u, c);
    requests_served++;
    return;
#else
    if (!demo_queue_send(&q_proxy_gate, &rx_msg)) {
        queue_busy++; /* cannot happen with run-to-idle; fail closed anyway */
        return;
    }
    microkit_notify(CH_PROXY_REQ);
    dispatch();
    uint64_t c = rs_pmccntr() - s;
    if (!tx_ready) {
        no_response++;
        return; /* no response produced: K1 times out (fail closed) */
    }
    tx_ready = 0;
    if (tx_msg.kind != DEMO_MESSAGE_HTTP_RESPONSE || tx_msg.length > GW_MSG_MAX) {
        no_response++;
        return;
    }
    send_response(rx_id, tx_msg.request_epoch, tx_msg.payload, tx_msg.length, c);
    requests_served++;
#endif
}

/* One channel message from K1. Anything that is not the in-order next
 * fragment of a message is counted and discards the partial message. */
void gw_on_frame(uint16_t type, const uint8_t *payload, uint32_t len)
{
    gw_frag_hdr_t h;

    if (type != GW_MSG_REQ_FRAG || len < sizeof(h)) {
        frames_rejected++;
        rx_active = 0;
        return;
    }
    memcpy(&h, payload, sizeof(h));
    const uint32_t n = len - (uint32_t)sizeof(h);
    if (h.offset == 0) {
        memset(&rx_msg, 0, sizeof(rx_msg));
        rx_active = 1;
        rx_id = h.msg_id;
        rx_total = h.total;
        rx_have = 0;
        rx_msg.kind = h.kind;
        rx_msg.request_epoch = h.epoch;
    }
    if (!rx_active || h.msg_id != rx_id || h.total != rx_total || h.offset != rx_have ||
        rx_total > GW_MSG_MAX || n > rx_total - rx_have || (n == 0 && rx_total != 0)) {
        frames_rejected++;
        rx_active = 0;
        return;
    }
    memcpy(rx_msg.payload + rx_have, payload + sizeof(h), n);
    rx_have += n;
    if (rx_have == rx_total) {
        rx_active = 0;
        rx_msg.length = rx_total; /* bytes actually received, not a K1 claim */
        serve_request(h.epoch);
    }
}

uint64_t gw_rejected_frames(void)
{
    return frames_rejected;
}

uint64_t gw_no_response(void)
{
    return no_response + queue_busy;
}

uint64_t gw_send_failures(void)
{
    return send_failures;
}

uint64_t gw_requests_served(void)
{
    return requests_served;
}

/* ---- Engine-level runs (D2 equivalence + extractor vectors, D3 engine-only) ---- */

#if MK_REAL_ENGINE
static uint64_t policy_digest(void)
{
    CompactPolicy p;
    memset(&p, 0, sizeof(p));
    if (engine_get_policy(&p) != 0) {
        return 0;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t b[1 + MAX_RULES * POLICY_RULE_SIZE];
    b[0] = p.num_rules;
    for (unsigned i = 0; i < MAX_RULES; i++) {
        uint8_t *r = b + 1 + i * POLICY_RULE_SIZE;
        write_u32_le(r, 0, p.rules[i].path_id);
        r[4] = p.rules[i].method;
        r[5] = p.rules[i].min_role;
        write_u16_le(r, 6, p.rules[i].required_scope);
    }
    for (unsigned i = 0; i < sizeof(b); i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

static void install_default_policy(void)
{
    CompactPolicy p;
    uint8_t body[POLICY_UPDATE_BODY_SIZE];
    const DecisionInput admin = { PATH_POLICY, METHOD_PUT, ROLE_ADMIN, UINT16_MAX, 0u };
    compact_policy_init_default(&p);
    memset(body, 0, sizeof(body));
    body[0] = p.num_rules;
    for (unsigned i = 0; i < MAX_RULES; i++) {
        uint8_t *r = body + 1 + i * POLICY_RULE_SIZE;
        write_u32_le(r, 0, p.rules[i].path_id);
        r[4] = p.rules[i].method;
        r[5] = p.rules[i].min_role;
        write_u16_le(r, 6, p.rules[i].required_scope);
    }
    UpdateResult u = engine_update(&admin, body, sizeof(body));
    put_str(u == UPDATE_APPLIED ? "gw: default policy installed, digest="
                                : "gw: default policy install FAILED, digest=");
    put_hex(policy_digest());
    put_str("\n");
}

static int cmp64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void c2_case(const char *name, const DecisionInput *in, int expected, int empty)
{
    static uint64_t cyc[C2_SAMPLES];
    volatile DecisionResult sink = DECISION_DENY;
    uint64_t mismatches = 0, sum = 0;

    for (unsigned i = 0; i < C2_WARMUP; i++) {
        if (!empty) {
            sink = engine_decide(in);
        }
    }
    uint64_t t0 = rs_cntpct();
    for (unsigned i = 0; i < C2_SAMPLES; i++) {
        uint64_t s = rs_pmccntr();
        if (!empty) {
            sink = engine_decide(in);
        }
        uint64_t e = rs_pmccntr();
        cyc[i] = e - s;
        sum += cyc[i];
        if (!empty && (int)sink != expected) {
            mismatches++;
        }
    }
    uint64_t ticks = rs_cntpct() - t0;
    qsort(cyc, C2_SAMPLES, sizeof(cyc[0]), cmp64);
    printf("C2_RESULT,engine=%s,case=%s,samples=%u,warmup=%u,unit=cycles,cpu_hz=1200000000,"
           "min=%llu,p50=%llu,p90=%llu,p99=%llu,max=%llu,mean=%llu,"
           "loop_cntpct_ticks=%llu,cntfrq=%llu,mismatches=%llu\n",
           MK_ENGINE_NAME, name, C2_SAMPLES, C2_WARMUP,
           (unsigned long long)cyc[0], (unsigned long long)cyc[C2_SAMPLES / 2 - 1],
           (unsigned long long)cyc[C2_SAMPLES * 9 / 10 - 1],
           (unsigned long long)cyc[C2_SAMPLES * 99 / 100 - 1],
           (unsigned long long)cyc[C2_SAMPLES - 1],
           (unsigned long long)(sum / C2_SAMPLES), (unsigned long long)ticks,
           (unsigned long long)rs_cntfrq(), (unsigned long long)mismatches);
}
#endif

/*
 * Called by policy_gate's init() right after engine_init(), where the x86
 * build runs its own timing harness (x86-only, rdtsc). On the board: D3
 * engine-only cycles (same inputs as before), D2 engine equivalence and the
 * F* extractor test vectors, then the default policy is reinstalled.
 */
int gateway_timing_run(void)
{
#if MK_REAL_ENGINE
    const PolicyRule first = gateway_benchmark_rule(0u);
    const PolicyRule last = gateway_benchmark_rule(GATEWAY_BENCHMARK_ACTIVE_RULES - 1u);
    const DecisionInput allow_first = {
        first.path_id, (uint8_t)(first.method & (uint8_t)(0u - first.method)),
        ROLE_ADMIN, UINT16_MAX, 0u };
    const DecisionInput allow_last = {
        last.path_id, (uint8_t)(last.method & (uint8_t)(0u - last.method)),
        ROLE_ADMIN, UINT16_MAX, 0u };
    const DecisionInput deny_path = {
        UINT32_C(0xfeedbeef), METHOD_GET, ROLE_ADMIN, UINT16_MAX, 0u };

    install_default_policy();
    c2_case("empty_bracket", &deny_path, DECISION_DENY, 1);
    c2_case("allow_first", &allow_first, DECISION_ALLOW, 0);
    c2_case("allow_last", &allow_last, DECISION_ALLOW, 0);
    c2_case("deny_path", &deny_path, DECISION_DENY, 0);
    fflush(stdout);
    pet_watchdog_k0();

    uint64_t t0 = rs_cntpct();
    put_str("d2: equivalence start engine=" MK_ENGINE_NAME "\n");
    int rc = equivalence_main();
    fflush(stdout);
    fflush(stderr);
    put_str("d2: equivalence rc=");
    put_dec((uint64_t)rc);
    put_str(" ms=");
    put_dec((rs_cntpct() - t0) * 1000u / rs_cntfrq());
    put_str("\n");
    pet_watchdog_k0();
    int xrc = extractor_test_main();
    fflush(stdout);
    printf("D2_EXTRACTOR,%s,rc=%d\n", xrc == 0 ? "EXTRACTOR_OK" : "EXTRACTOR_FAIL", xrc);
    fflush(stdout);
    install_default_policy(); /* the equivalence run leaves a random policy */
    return rc == 0 && xrc == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void gw_init(void)
{
    pg_demo_config = (DemoConfig){
        .input_vaddr = (uintptr_t)&q_proxy_gate, .output_vaddr = (uintptr_t)&q_gate_app,
        .input_channel = CH_GATE_IN, .output_channel = CH_GATE_OUT };
    pa_demo_config = (DemoConfig){
        .input_vaddr = (uintptr_t)&q_gate_app, .output_vaddr = (uintptr_t)&q_app_proxy,
        .input_channel = CH_APP_IN, .output_channel = CH_APP_OUT };
    pg_auth_config = (AuthConfig){
        .request_vaddr = (uintptr_t)&q_auth_req, .response_vaddr = (uintptr_t)&q_auth_rsp,
        .channel = CH_AUTH };
    au_auth_config = pg_auth_config;

    current_pd = PD_AUTH;
    au_init();
    current_pd = PD_APP;
    pa_init();
    current_pd = PD_GATE;
    pg_init(); /* engine_init + gateway_timing_run (D3 engine-only, D2) */
    current_pd = PD_PROXY;
    dispatch();
    put_str("gw: engine=" MK_ENGINE_NAME " gate pipeline ready\n");
}

#endif /* KERNEL_ID == 0 */
