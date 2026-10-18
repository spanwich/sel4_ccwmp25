/*
 * K0 only: a minimal OP-TEE SMC-ABI client that sends SCMI messages to the
 * OP-TEE SCMI server (PTA a8cfe406-d4f5-4a2e-9f8d-a25dc754c099, agent@1 =
 * non-secure A35, channel 0, "linaro,scmi-optee" message transport), and N1:
 * turning on the ETH1 kernel clocks through it.
 *
 * Message memory is one 4 KiB page of K0 RAM (non-secure DDR; OP-TEE runs
 * with CFG_CORE_DYN_SHM=y and maps it per call). Layout:
 *   +0x000 struct optee_msg_arg (+ up to 4 params)
 *   +0x400 SCMI request   +0x800 SCMI response
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>

#include "rs.h"

#if KERNEL_ID == 0

#define OPTEE_SMC_CALL_WITH_ARG         0x32000004u
#define OPTEE_SMC_CALL_RETURN_FROM_RPC  0x32000003u
#define OPTEE_SMC_RETURN_OK             0x0u
#define OPTEE_SMC_RETURN_RPC_PREFIX     0xffff0000u
#define OPTEE_SMC_RPC_FUNC_FOREIGN_INTR 4u

#define OPTEE_MSG_CMD_OPEN_SESSION      0u
#define OPTEE_MSG_CMD_INVOKE_COMMAND    1u
#define OPTEE_MSG_ATTR_META             0x100u
#define OPTEE_MSG_ATTR_VALUE_INPUT      0x1u
#define OPTEE_MSG_ATTR_TMEM_INPUT       0x9u
#define OPTEE_MSG_ATTR_TMEM_OUTPUT      0xau
#define OPTEE_MSG_ATTR_NONCONTIG        0x200u /* buf_ptr = page list | offset */
#define TEE_LOGIN_REE_KERNEL            0x80000000u

#define PTA_SCMI_CMD_PROCESS_MSG_CHANNEL 4u
#define SCMI_CHANNEL_A35_NS             0u

#define SCMI_PROTOCOL_CLOCK             0x14u
#define SCMI_CLOCK_ATTRIBUTES           0x3u
#define SCMI_CLOCK_CONFIG_SET           0x7u

/* OP-TEE clock IDs exported to the non-secure agent (dt-bindings stm32mp25-clks.h) */
#define CK_SCMI_FLEXGEN_54  54u /* ck_ker_eth1 / ethsw (PLL6) */
#define CK_SCMI_FLEXGEN_56  56u /* ck_ker_eth1ptp */
#define CK_SCMI_ICN_LS_MCU  64u /* ETH1 bus clock parent */

/* RCC_ETH1CFGR: gates follow RIFSC ID 60 (ETH1, non-secure). */
#define RCC_BASE            0x44200000ull
#define RCC_ETH1CFGR        0x7f0u
#define ETH1CFGR_RST        (1u << 0)
#define ETH1CFGR_MACEN      (1u << 1)
#define ETH1CFGR_STPEN      (1u << 4)
#define ETH1CFGR_EN         (1u << 5)
#define ETH1CFGR_TXEN       (1u << 8)
#define ETH1CFGR_RXEN       (1u << 10)
#define ETH1CFGR_ALL_EN     (ETH1CFGR_MACEN | ETH1CFGR_STPEN | ETH1CFGR_EN | \
                             ETH1CFGR_TXEN | ETH1CFGR_RXEN)

struct optee_msg_param {
    uint64_t attr;
    uint64_t a, b, c; /* value.{a,b,c} or tmem.{buf_ptr,size,shm_ref} */
};

struct optee_msg_arg {
    uint32_t cmd, func, session, cancel_id, pad, ret, ret_origin, num_params;
    struct optee_msg_param params[4];
};

static volatile uint8_t *shm;
static uint64_t shm_pa;
static uint64_t list_pa; /* page list: entry 0 = shm_pa (OP-TEE has no static SHM) */
static uint32_t session;
static uint32_t scmi_token;

static struct optee_msg_arg *arg(void)
{
    return (struct optee_msg_arg *)(uintptr_t)shm;
}

/* One OPTEE_SMC_CALL_WITH_ARG, resuming after foreign-interrupt RPCs. */
static uint32_t optee_call(void)
{
    seL4_ARM_SMCContext req = {
        .x0 = OPTEE_SMC_CALL_WITH_ARG,
        .x1 = shm_pa >> 32,
        .x2 = shm_pa & 0xffffffffu,
    };
    seL4_ARM_SMCContext resp;

    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned rpcs = 0; rpcs < 1000; rpcs++) {
        memset(&resp, 0, sizeof(resp));
        smc_call(&req, &resp);
        uint32_t r = (uint32_t)resp.x0;
        if (r == OPTEE_SMC_RETURN_OK) {
            __asm__ volatile("dsb sy" ::: "memory");
            return arg()->ret;
        }
        if ((r & 0xffff0000u) != OPTEE_SMC_RETURN_RPC_PREFIX ||
            (r & 0xffffu) != OPTEE_SMC_RPC_FUNC_FOREIGN_INTR) {
            put_str("optee: unexpected SMC return ");
            put_hex(r);
            put_str("\n");
            halt("optee_call", r);
        }
        /* A non-secure interrupt is pending: let it be taken, then resume. */
        req.x0 = OPTEE_SMC_CALL_RETURN_FROM_RPC;
        req.x1 = resp.x1;
        req.x2 = resp.x2;
        req.x3 = resp.x3;
    }
    halt("optee_call: RPC storm", 0);
    return 0;
}

static void optee_scmi_open(void)
{
    static const uint8_t uuid[16] = {
        0xa8, 0xcf, 0xe4, 0x06, 0xd4, 0xf5, 0x4a, 0x2e,
        0x9f, 0x8d, 0xa2, 0x5d, 0xc7, 0x54, 0xc0, 0x99,
    };
    seL4_CPtr frame = alloc_object(seL4_ARM_SmallPageObject, seL4_PageBits,
                                   "optee shm page");
    /* Cacheable like OP-TEE's own mapping of NS shm; same core, so coherent. */
    map_frame_attr(frame, RS_VADDR_OPTEE_SHM, seL4_ReadWrite,
                   seL4_ARM_Default_VMAttributes | seL4_ARM_ExecuteNever);
    shm = (volatile uint8_t *)RS_VADDR_OPTEE_SHM;
    shm_pa = seL4_ARM_Page_GetAddress(frame).paddr;

    /* Memrefs must be non-contiguous page lists (CFG_CORE_RESERVED_SHM=n).
     * Both buffers live in the shm page, so one list page serves both: its
     * entry 0 is shm_pa, and buf_ptr's low 12 bits give the offset. */
    seL4_CPtr lframe = alloc_object(seL4_ARM_SmallPageObject, seL4_PageBits,
                                    "optee page list");
    map_frame_attr(lframe, RS_VADDR_OPTEE_SHM + 0x1000, seL4_ReadWrite,
                   seL4_ARM_Default_VMAttributes | seL4_ARM_ExecuteNever);
    volatile uint64_t *list = (volatile uint64_t *)(RS_VADDR_OPTEE_SHM + 0x1000);
    for (unsigned i = 0; i < 512; i++) {
        list[i] = 0;
    }
    list[0] = shm_pa;
    list_pa = seL4_ARM_Page_GetAddress(lframe).paddr;

    struct optee_msg_arg *a = arg();
    memset(a, 0, sizeof(*a));
    a->cmd = OPTEE_MSG_CMD_OPEN_SESSION;
    a->num_params = 2;
    a->params[0].attr = OPTEE_MSG_ATTR_META | OPTEE_MSG_ATTR_VALUE_INPUT;
    memcpy(&a->params[0].a, uuid, sizeof(uuid)); /* value.a,b = UUID octets */
    a->params[1].attr = OPTEE_MSG_ATTR_META | OPTEE_MSG_ATTR_VALUE_INPUT;
    a->params[1].c = TEE_LOGIN_REE_KERNEL;
    uint32_t ret = optee_call();
    if (ret != 0) {
        put_str("optee: open SCMI PTA ret=");
        put_hex(ret);
        put_str(" origin=");
        put_dec(a->ret_origin);
        put_str("\n");
        halt("open SCMI PTA", ret);
    }
    session = a->session;
    put_str("optee: SCMI PTA session ");
    put_dec(session);
    put_str(", shm pa ");
    put_hex(shm_pa);
    put_str("\n");
}

/* Send one SCMI message; returns the response length, response at resp. */
static uint32_t scmi_xfer(uint32_t protocol, uint32_t msg_id,
                          const uint32_t *payload, unsigned n_words,
                          const volatile uint32_t **resp)
{
    volatile uint32_t *in = (volatile uint32_t *)(shm + 0x400);
    volatile uint32_t *out = (volatile uint32_t *)(shm + 0x800);

    in[0] = msg_id | (protocol << 10) | ((scmi_token++ & 0x3ffu) << 18);
    for (unsigned i = 0; i < n_words; i++) {
        in[1 + i] = payload[i];
    }
    struct optee_msg_arg *a = arg();
    memset(a, 0, sizeof(*a));
    a->cmd = OPTEE_MSG_CMD_INVOKE_COMMAND;
    a->func = PTA_SCMI_CMD_PROCESS_MSG_CHANNEL;
    a->session = session;
    a->num_params = 3;
    a->params[0].attr = OPTEE_MSG_ATTR_VALUE_INPUT;
    a->params[0].a = SCMI_CHANNEL_A35_NS;
    a->params[1].attr = OPTEE_MSG_ATTR_TMEM_INPUT | OPTEE_MSG_ATTR_NONCONTIG;
    a->params[1].a = list_pa | 0x400;
    a->params[1].b = 4u * (1 + n_words);
    a->params[1].c = 1; /* shm_ref cookie */
    a->params[2].attr = OPTEE_MSG_ATTR_TMEM_OUTPUT | OPTEE_MSG_ATTR_NONCONTIG;
    a->params[2].a = list_pa | 0x800;
    a->params[2].b = 0x400;
    a->params[2].c = 2;
    uint32_t ret = optee_call();
    if (ret != 0) {
        halt("SCMI PROCESS_MSG_CHANNEL", ret);
    }
    *resp = out;
    return (uint32_t)a->params[2].b;
}

/* Returns SCMI status; *enabled from CLOCK_ATTRIBUTES bit 0. */
static int32_t scmi_clock_attributes(uint32_t id, unsigned *enabled)
{
    const volatile uint32_t *r;
    uint32_t len = scmi_xfer(SCMI_PROTOCOL_CLOCK, SCMI_CLOCK_ATTRIBUTES, &id, 1, &r);
    if (len < 12) {
        return -1000;
    }
    *enabled = r[2] & 1u;
    return (int32_t)r[1];
}

static int32_t scmi_clock_enable(uint32_t id)
{
    const volatile uint32_t *r;
    uint32_t payload[2] = { id, 1u /* enable */ };
    uint32_t len = scmi_xfer(SCMI_PROTOCOL_CLOCK, SCMI_CLOCK_CONFIG_SET, payload, 2, &r);
    return len >= 8 ? (int32_t)r[1] : -1000;
}

static void clock_on(uint32_t id, const char *name)
{
    unsigned before = 0, after = 0;
    int32_t st = scmi_clock_attributes(id, &before);
    int32_t en = scmi_clock_enable(id);
    int32_t st2 = scmi_clock_attributes(id, &after);

    put_str("scmi: clock ");
    put_str(name);
    put_str(" (");
    put_dec(id);
    put_str(") attr_status=");
    put_dec((uint32_t)st);
    put_str(" enabled ");
    put_dec(before);
    put_str(" -> enable status=");
    put_dec((uint32_t)en);
    put_str(" -> enabled ");
    put_dec(after);
    put_str("\n");
    if (en != 0 || st2 != 0 || !after) {
        halt("SCMI clock enable", id);
    }
}

void eth1_clocks_enable(void)
{
    optee_scmi_open();
    clock_on(CK_SCMI_ICN_LS_MCU, "ICN_LS_MCU");
    clock_on(CK_SCMI_FLEXGEN_54, "FLEXGEN_54/ETH1_KER");
    clock_on(CK_SCMI_FLEXGEN_56, "FLEXGEN_56/ETH1_PTP");

    /* Only this one register of the RCC page is touched: most of RCC is
     * secure, and a stray access would raise an IAC (OP-TEE panics). */
    volatile uint32_t *rcc = map_device_page(RCC_BASE + RCC_ETH1CFGR, RS_VADDR_RCC);
    volatile uint32_t *cfgr = (volatile uint32_t *)((uintptr_t)rcc + RCC_ETH1CFGR);
    uint32_t v0 = *cfgr;
    *cfgr = (v0 | ETH1CFGR_ALL_EN) | ETH1CFGR_RST; /* clocks on, held in reset */
    rs_udelay(10);
    *cfgr = (v0 | ETH1CFGR_ALL_EN) & ~ETH1CFGR_RST;
    uint32_t v1 = *cfgr;
    put_str("rcc: ETH1CFGR ");
    put_hex(v0);
    put_str(" -> ");
    put_hex(v1);
    put_str((v1 & (ETH1CFGR_ALL_EN | ETH1CFGR_RST)) == ETH1CFGR_ALL_EN ?
            " (gates on, out of reset)\n" : " UNEXPECTED\n");
}

#endif /* KERNEL_ID == 0 */
