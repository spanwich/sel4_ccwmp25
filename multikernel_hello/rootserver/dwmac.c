/*
 * K1 only: STM32MP25 ETH1 (Synopsys DWMAC 5.10a, "stmmac") bring-up.
 *
 * Minimal port of what Linux dwmac-stm32 + stmmac + pinctrl-stm32 do for the
 * CCMP25-DVK ETH1 port (RJ45 J22): RGMII-ID to a Marvell 88E1512 at MDIO
 * address 0, PHY reset on GPIOB2 (active low). Clocks are already on: K0
 * enabled them over SCMI before K1 attached to the channel (N1).
 *
 * Every register touched here is non-secure on this board: ETH1 is RIFSC ID
 * 60 NSEC, all pins used are RIF_NSEC in OP-TEE's ccmp25-dvk-rif.dtsi, and
 * Linux writes the same SYSCFG register from the non-secure world.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>

#include "rs.h"
#include "dwmac.h"

#if KERNEL_ID == 1

/* ---- MMIO ---- */
#define ETH1_PADDR      0x482c0000ull /* 16 KiB: MAC, MTL, DMA */
#define SYSCFG_PADDR    0x44230000ull
#define SYSCFG_ETH1CR   0x3000u
#define ETHCR_SEL_RGMII (1u << 4)
#define GPIO_PADDR(p)   (0x44240000ull + 0x10000ull * (unsigned)((p) - 'A'))

#define GPIO_MODER   0x00
#define GPIO_OTYPER  0x04
#define GPIO_OSPEEDR 0x08
#define GPIO_PUPDR   0x0c
#define GPIO_BSRR    0x18
#define GPIO_AFRL    0x20
#define GPIO_ADVCFGRL 0x48
#define ADVCFG_DE    (1u << 1) /* st,io-clk-edge */
#define ADVCFG_RET   (1u << 3) /* st,io-retime */

/* DWMAC 5.x register offsets (stmmac dwmac4.h / dwmac4_dma.h) */
#define MAC_CONFIG          0x0000
#define MAC_PACKET_FILTER   0x0008
#define MAC_Q0_TX_FLOW_CTRL 0x0070
#define MAC_RXQ_CTRL0       0x00a0
#define MAC_VERSION         0x0110
#define MAC_HW_FEATURE1     0x0120
#define MAC_MDIO_ADDRESS    0x0200
#define MAC_MDIO_DATA       0x0204
#define MAC_ADDR_HIGH0      0x0300
#define MAC_ADDR_LOW0       0x0304
#define MTL_TXQ0_OP_MODE    0x0d00
#define MTL_RXQ0_OP_MODE    0x0d30
#define DMA_MODE            0x1000
#define DMA_SYSBUS_MODE     0x1004
#define DMA_CH0_CONTROL     0x1100
#define DMA_CH0_TX_CONTROL  0x1104
#define DMA_CH0_RX_CONTROL  0x1108
#define DMA_CH0_TXDESC_HI   0x1110
#define DMA_CH0_TXDESC_LO   0x1114
#define DMA_CH0_RXDESC_HI   0x1118
#define DMA_CH0_RXDESC_LO   0x111c
#define DMA_CH0_TXDESC_TAIL 0x1120
#define DMA_CH0_RXDESC_TAIL 0x1128
#define DMA_CH0_TXDESC_RING 0x112c
#define DMA_CH0_RXDESC_RING 0x1130
#define DMA_CH0_INTR_ENA    0x1134
#define DMA_CH0_STATUS      0x1160

#define MDIO_GB        (1u << 0)
#define MDIO_GOC_READ  (3u << 2)
#define MDIO_GOC_WRITE (1u << 2)
#define MDIO_CR_SHIFT  8
#define MDIO_CR        5u /* CSR clock 250-300 MHz: /124 (safe upper bound) */
#define MDIO_RDA_SHIFT 16
#define MDIO_PA_SHIFT  21

#define PHY_ADDR 0u

static volatile uint8_t *eth;
static volatile uint8_t *gpio[11];

static inline uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(eth + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(eth + off) = v;
}

static volatile uint32_t *gpio_reg(char port, uint32_t off)
{
    unsigned i = (unsigned)(port - 'A');
    if (!gpio[i]) {
        gpio[i] = map_device_page(GPIO_PADDR(port), RS_VADDR_GPIO_BASE + i * 0x1000ul);
    }
    return (volatile uint32_t *)(gpio[i] + off);
}

static void rmw(volatile uint32_t *r, uint32_t mask, uint32_t val)
{
    *r = (*r & ~mask) | (val & mask);
}

/* Pin to alternate function af, push-pull, no pull, given slew (0-3) and
 * optional io-retime/clk-edge flags (ADVCFGR, 4 bits per pin). */
static void pin_af(char port, unsigned pin, unsigned af, unsigned slew, uint32_t adv)
{
    rmw(gpio_reg(port, GPIO_AFRL + (pin / 8) * 4), 0xfu << ((pin % 8) * 4),
        af << ((pin % 8) * 4));
    rmw(gpio_reg(port, GPIO_OTYPER), 1u << pin, 0);
    rmw(gpio_reg(port, GPIO_PUPDR), 3u << (pin * 2), 0);
    rmw(gpio_reg(port, GPIO_OSPEEDR), 3u << (pin * 2), slew << (pin * 2));
    rmw(gpio_reg(port, GPIO_ADVCFGRL + (pin / 8) * 4),
        (ADVCFG_DE | ADVCFG_RET) << ((pin % 8) * 4), adv << ((pin % 8) * 4));
    rmw(gpio_reg(port, GPIO_MODER), 3u << (pin * 2), 2u << (pin * 2));
}

static void pin_out(char port, unsigned pin, int level)
{
    *gpio_reg(port, GPIO_BSRR) = level ? (1u << pin) : (1u << (pin + 16));
    rmw(gpio_reg(port, GPIO_OTYPER), 1u << pin, 0);
    rmw(gpio_reg(port, GPIO_MODER), 3u << (pin * 2), 1u << (pin * 2));
}

/* Linux st/stm32mp25-pinctrl.dtsi eth1_rgmii_pins_a + eth1_mdio_pins_a */
static void eth1_pinmux(void)
{
    const uint32_t rt = ADVCFG_RET | ADVCFG_DE;
    /* pins1: TXD0-3, TX_CTL: AF10, slew 3, io-retime, io-clk-edge */
    pin_af('A', 15, 10, 3, rt);
    pin_af('C', 1, 10, 3, rt);
    pin_af('H', 10, 10, 3, rt);
    pin_af('H', 11, 10, 3, rt);
    pin_af('A', 13, 10, 3, rt);
    /* pins2: CLK125 (AF10), GTX_CLK (AF12), slew 3 */
    pin_af('H', 9, 10, 3, 0);
    pin_af('C', 0, 12, 3, 0);
    /* pins3: RXD0-3, RX_CTL: AF10, io-retime, io-clk-edge */
    pin_af('F', 1, 10, 0, rt);
    pin_af('C', 2, 10, 0, rt);
    pin_af('H', 12, 10, 0, rt);
    pin_af('H', 13, 10, 0, rt);
    pin_af('A', 11, 10, 0, rt);
    /* pins4: RX_CLK */
    pin_af('A', 14, 10, 0, 0);
    /* MDIO: MDC slew 2, MDIO slew 0 */
    pin_af('F', 0, 10, 2, 0);
    pin_af('F', 2, 10, 0, 0);
}

static int mdio_wait(void)
{
    for (unsigned i = 0; i < 100000; i++) {
        if (!(rd(MAC_MDIO_ADDRESS) & MDIO_GB)) {
            return 0;
        }
    }
    return -1;
}

int mdio_read(unsigned reg)
{
    if (mdio_wait()) {
        return -1;
    }
    wr(MAC_MDIO_ADDRESS, (PHY_ADDR << MDIO_PA_SHIFT) | (reg << MDIO_RDA_SHIFT) |
       (MDIO_CR << MDIO_CR_SHIFT) | MDIO_GOC_READ | MDIO_GB);
    if (mdio_wait()) {
        return -1;
    }
    return (int)(rd(MAC_MDIO_DATA) & 0xffffu);
}

int mdio_write(unsigned reg, uint16_t val)
{
    if (mdio_wait()) {
        return -1;
    }
    wr(MAC_MDIO_DATA, val);
    wr(MAC_MDIO_ADDRESS, (PHY_ADDR << MDIO_PA_SHIFT) | (reg << MDIO_RDA_SHIFT) |
       (MDIO_CR << MDIO_CR_SHIFT) | MDIO_GOC_WRITE | MDIO_GB);
    return mdio_wait();
}

void *eth1_regs(void)
{
    return (void *)eth;
}

/* N2: pinmux, RGMII select, PHY reset, MAC version and PHY ID. */
int eth1_probe(void)
{
    eth = map_device_page(ETH1_PADDR, RS_VADDR_ETH1);
    for (unsigned i = 1; i < 4; i++) {
        map_device_page(ETH1_PADDR + i * 0x1000ull, RS_VADDR_ETH1 + i * 0x1000ul);
    }
    put_str("eth: MAC_VERSION=");
    put_hex(rd(MAC_VERSION));
    put_str("\n");

    eth1_pinmux();

    volatile uint8_t *syscfg = map_device_page(SYSCFG_PADDR + SYSCFG_ETH1CR, RS_VADDR_SYSCFG);
    volatile uint32_t *ethcr = (volatile uint32_t *)(syscfg + SYSCFG_ETH1CR % 0x1000);
    uint32_t c0 = *ethcr;
    *ethcr = ETHCR_SEL_RGMII; /* RGMII, 125 MHz from the PHY (no ETH_CLK_SEL) */
    put_str("eth: SYSCFG_ETH1CR ");
    put_hex(c0);
    put_str(" -> ");
    put_hex(*ethcr);
    put_str("\n");

    /* PHY reset (Digi DT: GPIOB2 active low, 1 ms assert, 2 ms deassert) */
    pin_out('B', 2, 0);
    rs_udelay(1000);
    pin_out('B', 2, 1);
    rs_udelay(2000);

    int id1 = mdio_read(2), id2 = mdio_read(3);
    uint32_t id = ((uint32_t)id1 << 16) | (uint32_t)id2;
    put_str("phy: id=");
    put_hex(id);
    put_str(id1 < 0 || id2 < 0 ? " (MDIO timeout)\n" :
            (id & 0xfffffff0u) == 0x01410dd0u ? " (Marvell 88E1512)\n" : " (UNEXPECTED)\n");
    return (id & 0xfffffff0u) == 0x01410dd0u ? 0 : -1;
}

/* Copper link from the 88E1512 copper-specific status (page 0, reg 17). */
int eth1_link(unsigned *speed, unsigned *full)
{
    int s = mdio_read(17);
    if (s < 0 || !(s & (1 << 10))) {
        return 0;
    }
    *speed = ((s >> 14) & 3) == 2 ? 1000 : ((s >> 14) & 3) == 1 ? 100 : 10;
    *full = (s >> 13) & 1;
    return 1;
}


/* ---- N3: DMA rings in the ETH1 DMA window (K1 RAM top 8 MiB) ----
 *
 * The window 0xab800000..0xac000000 is outside K1's DTS memory, so it is a
 * device untyped carved at exact addresses and mapped Device-nGnRnE: the CPU
 * and the DMA always agree, and only aligned 32-bit accesses are used. With
 * the N5 firmware, RISAF4 region 11 is the only DDR ETH1 (CID4) may reach.
 */
#define N_TX 32
#define N_RX 64
#define BUF_SZ 2048u
#define DESC_TX_OFF 0x0000u
#define DESC_RX_OFF 0x1000u
#define RXBUF_OFF   0x10000u
#define TXBUF_OFF   0x40000u

#define DES3_OWN   (1u << 31)
#define RDES3_IOC  (1u << 30)
#define RDES3_BUF1V (1u << 24)
#define RDES3_FD   (1u << 29)
#define RDES3_LD   (1u << 28)
#define RDES3_ES   (1u << 15)
#define RDES3_PL   0x7fffu
#define TDES2_IOC  (1u << 31)
#define TDES3_FD   (1u << 29)
#define TDES3_LD   (1u << 28)

static volatile uint32_t *dma_va;
static uint64_t dma_pa;
static unsigned rx_next, tx_next, tx_clean;
struct eth1_stats eth1_stats;

static inline volatile uint32_t *desc(uint32_t off, unsigned i)
{
    return dma_va + (off + 16u * i) / 4;
}

void eth1_dma_init(void *va, uint64_t pa, const uint8_t mac[6])
{
    dma_va = va;
    dma_pa = pa;

    /* 88E1512 rgmii-id: PHY adds RX and TX clock delays (page 2, reg 21,
     * bits 5/4), as Linux m88e1121_config_aneg_rgmii_delays does. */
    mdio_write(22, 2);
    int mscr = mdio_read(21);
    mdio_write(21, (uint16_t)((mscr < 0 ? 0 : mscr) | (1u << 5) | (1u << 4)));
    mdio_write(22, 0);
    mdio_write(0, 0x9140); /* BMCR: reset + autoneg + 1000/full advertised */

    wr(DMA_MODE, 1); /* software reset; needs the RGMII RX clock from the PHY */
    uint64_t until = rs_cntpct() + rs_cntfrq();
    while ((rd(DMA_MODE) & 1) && rs_cntpct() < until) {
    }
    put_str((rd(DMA_MODE) & 1) ? "eth: DMA reset TIMEOUT\n" : "eth: DMA reset done\n");

    wr(DMA_SYSBUS_MODE, (1u << 12) | 0xe); /* AAL, BLEN 4/8/16 */
    for (unsigned i = 0; i < N_TX; i++) {
        volatile uint32_t *d = desc(DESC_TX_OFF, i);
        d[0] = d[1] = d[2] = d[3] = 0;
    }
    for (unsigned i = 0; i < N_RX; i++) {
        volatile uint32_t *d = desc(DESC_RX_OFF, i);
        d[0] = (uint32_t)(dma_pa + RXBUF_OFF + i * BUF_SZ);
        d[1] = 0;
        d[2] = 0;
        d[3] = DES3_OWN | RDES3_IOC | RDES3_BUF1V;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    wr(DMA_CH0_TX_CONTROL, 16u << 16);
    wr(DMA_CH0_RX_CONTROL, (16u << 16) | (BUF_SZ << 1));
    wr(DMA_CH0_TXDESC_RING, N_TX - 1);
    wr(DMA_CH0_RXDESC_RING, N_RX - 1);
    wr(DMA_CH0_TXDESC_HI, (uint32_t)((dma_pa + DESC_TX_OFF) >> 32));
    wr(DMA_CH0_TXDESC_LO, (uint32_t)(dma_pa + DESC_TX_OFF));
    wr(DMA_CH0_RXDESC_HI, (uint32_t)((dma_pa + DESC_RX_OFF) >> 32));
    wr(DMA_CH0_RXDESC_LO, (uint32_t)(dma_pa + DESC_RX_OFF));
    wr(DMA_CH0_TXDESC_TAIL, (uint32_t)(dma_pa + DESC_TX_OFF));
    wr(DMA_CH0_RXDESC_TAIL, (uint32_t)(dma_pa + DESC_RX_OFF + 16u * N_RX));

    /* MTL queue 0: store-and-forward, sized from HW_FEATURE1 FIFO sizes */
    uint32_t hw1 = rd(MAC_HW_FEATURE1);
    uint32_t rxfifo = 128u << (hw1 & 0x1f), txfifo = 128u << ((hw1 >> 6) & 0x1f);
    wr(MTL_TXQ0_OP_MODE, ((txfifo / 256 - 1) << 16) | (2u << 2) | (1u << 1));
    wr(MTL_RXQ0_OP_MODE, ((rxfifo / 256 - 1) << 20) | (1u << 5));
    wr(MAC_RXQ_CTRL0, 2); /* RX queue 0 enabled (generic) */

    wr(MAC_ADDR_HIGH0, (1u << 31) | ((uint32_t)mac[5] << 8) | mac[4]);
    wr(MAC_ADDR_LOW0, ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
       ((uint32_t)mac[1] << 8) | mac[0]);
    wr(MAC_PACKET_FILTER, 0); /* perfect filter + broadcast */

    wr(DMA_CH0_STATUS, 0xffffffffu);
    wr(DMA_CH0_INTR_ENA, (1u << 15) | (1u << 6) | (1u << 0)); /* NIE, RIE, TIE */
    wr(DMA_CH0_TX_CONTROL, rd(DMA_CH0_TX_CONTROL) | 1); /* ST */
    wr(DMA_CH0_RX_CONTROL, rd(DMA_CH0_RX_CONTROL) | 1); /* SR */
    put_str("eth: rx fifo ");
    put_dec(rxfifo);
    put_str(" B, tx fifo ");
    put_dec(txfifo);
    put_str(" B, rings at ");
    put_hex(dma_pa);
    put_str("\n");
}

/* Program MAC speed/duplex for the PHY's link and enable TX/RX. */
void eth1_mac_link(unsigned speed, unsigned full)
{
    uint32_t c = rd(MAC_CONFIG) & ~((1u << 15) | (1u << 14) | (1u << 13));
    if (speed == 100) {
        c |= (1u << 15) | (1u << 14);
    } else if (speed == 10) {
        c |= 1u << 15;
    }
    if (full) {
        c |= 1u << 13;
    }
    c |= (1u << 21) | (1u << 20) | 3u; /* CST, ACS: strip FCS/pad; TE, RE */
    wr(MAC_CONFIG, c);
}

/* Aligned word copies between private memory and the Device-mapped window. */
static void to_dma(volatile uint32_t *dst, const uint8_t *src, unsigned len)
{
    for (unsigned i = 0; i < len; i += 4) {
        uint32_t w = 0;
        memcpy(&w, src + i, len - i >= 4 ? 4 : len - i);
        dst[i / 4] = w;
    }
}

static void from_dma(uint8_t *dst, const volatile uint32_t *src, unsigned len)
{
    for (unsigned i = 0; i < len; i += 4) {
        uint32_t w = src[i / 4];
        memcpy(dst + i, &w, len - i >= 4 ? 4 : len - i);
    }
}

int eth1_tx(const uint8_t *frame, unsigned len)
{
    if (len > BUF_SZ) {
        eth1_stats.tx_err++;
        return -1;
    }
    /* reclaim completed descriptors */
    while (tx_clean != tx_next && !(desc(DESC_TX_OFF, tx_clean % N_TX)[3] & DES3_OWN)) {
        tx_clean++;
    }
    if (tx_next - tx_clean >= N_TX) {
        eth1_stats.tx_full++;
        return -1;
    }
    unsigned i = tx_next % N_TX;
    volatile uint32_t *buf = dma_va + (TXBUF_OFF + i * BUF_SZ) / 4;
    to_dma(buf, frame, len);
    volatile uint32_t *d = desc(DESC_TX_OFF, i);
    d[0] = (uint32_t)(dma_pa + TXBUF_OFF + i * BUF_SZ);
    d[1] = 0;
    d[2] = len & 0x3fffu;
    __asm__ volatile("dsb sy" ::: "memory");
    d[3] = DES3_OWN | TDES3_FD | TDES3_LD | len;
    __asm__ volatile("dsb sy" ::: "memory");
    tx_next++;
    wr(DMA_CH0_TXDESC_TAIL, (uint32_t)(dma_pa + DESC_TX_OFF + 16u * ((tx_next) % N_TX)));
    eth1_stats.tx++;
    return 0;
}

/* Deliver every completed RX frame to cb; returns the number delivered. */
unsigned eth1_rx(void (*cb)(const uint8_t *frame, unsigned len))
{
    static uint8_t frame[BUF_SZ];
    unsigned n = 0;

    uint32_t st = rd(DMA_CH0_STATUS);
    wr(DMA_CH0_STATUS, st); /* W1C */
    if (st & ((1u << 13) | (1u << 12))) { /* FBE / ETI-class bus errors */
        eth1_stats.dma_err++;
    }
    for (;;) {
        unsigned i = rx_next % N_RX;
        volatile uint32_t *d = desc(DESC_RX_OFF, i);
        uint32_t d3 = d[3];
        if (d3 & DES3_OWN) {
            break;
        }
        unsigned len = d3 & RDES3_PL;
        if ((d3 & RDES3_ES) || !(d3 & RDES3_FD) || !(d3 & RDES3_LD) || len > BUF_SZ) {
            eth1_stats.rx_err++;
        } else {
            from_dma(frame, dma_va + (RXBUF_OFF + i * BUF_SZ) / 4, len);
            eth1_stats.rx++;
            cb(frame, len);
            n++;
        }
        d[0] = (uint32_t)(dma_pa + RXBUF_OFF + i * BUF_SZ);
        d[1] = 0;
        d[2] = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        d[3] = DES3_OWN | RDES3_IOC | RDES3_BUF1V;
        rx_next++;
        wr(DMA_CH0_RXDESC_TAIL, (uint32_t)(dma_pa + DESC_RX_OFF + 16u * (i + 1)));
    }
    return n;
}

#endif /* KERNEL_ID == 1 */
