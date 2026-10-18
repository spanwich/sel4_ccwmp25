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

#endif /* KERNEL_ID == 1 */
