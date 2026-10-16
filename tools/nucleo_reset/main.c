/*
 * Nucleo-F767ZI remote reset for the CCMP25-DVK.
 *
 * PA3 (Arduino A0) is open-drain, wired to CCMP25 J6 pin 2 (NRST).
 * GND wired to J6 pin 3. Commands over ST-LINK VCP (USART3, 115200 8N1):
 *   'r'  pulse NRST low for 200 ms   -> "RESET\r\n"
 *   '?'  liveness check               -> "OK\r\n"
 * Bare metal, HSI 16 MHz, no HAL.
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

#define RCC_AHB1ENR   REG(0x40023830)
#define RCC_APB1ENR   REG(0x40023840)

#define GPIOA         0x40020000
#define GPIOD         0x40020C00
#define MODER(p)      REG((p) + 0x00)
#define OTYPER(p)     REG((p) + 0x04)
#define BSRR(p)       REG((p) + 0x18)
#define AFRH(p)       REG((p) + 0x24)

#define USART3        0x40004800
#define U_CR1         REG(USART3 + 0x00)
#define U_BRR         REG(USART3 + 0x0C)
#define U_ISR         REG(USART3 + 0x1C)
#define U_ICR         REG(USART3 + 0x20)
#define U_RDR         REG(USART3 + 0x24)
#define U_TDR         REG(USART3 + 0x28)

#define SYST_CSR      REG(0xE000E010)
#define SYST_RVR      REG(0xE000E014)
#define SYST_CVR      REG(0xE000E018)

#define NRST_PIN      3

static void delay_ms(uint32_t ms)
{
    SYST_RVR = 16000 - 1;
    SYST_CVR = 0;
    SYST_CSR = 5; /* processor clock, enabled, no interrupt */
    while (ms--)
        while (!(SYST_CSR & (1u << 16)))
            ;
    SYST_CSR = 0;
}

static void putc_(char c)
{
    while (!(U_ISR & (1u << 7)))
        ;
    U_TDR = (uint32_t)c;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void nrst_release(void) { BSRR(GPIOA) = 1u << NRST_PIN; }
static void nrst_assert(void)  { BSRR(GPIOA) = 1u << (NRST_PIN + 16); }

int main(void)
{
    RCC_AHB1ENR |= (1u << 0) | (1u << 3); /* GPIOA, GPIOD */
    RCC_APB1ENR |= (1u << 18);            /* USART3 */
    (void)RCC_APB1ENR;

    /* PA3: open-drain output, released (high-Z) before switching mode */
    nrst_release();
    OTYPER(GPIOA) |= 1u << NRST_PIN;
    MODER(GPIOA) = (MODER(GPIOA) & ~(3u << (NRST_PIN * 2))) | (1u << (NRST_PIN * 2));

    /* PD8 TX / PD9 RX, AF7 */
    MODER(GPIOD) = (MODER(GPIOD) & ~(0xFu << 16)) | (0xAu << 16);
    AFRH(GPIOD) = (AFRH(GPIOD) & ~0xFFu) | 0x77u;

    U_BRR = 16000000 / 115200;
    U_CR1 = (1u << 3) | (1u << 2) | 1u; /* TE, RE, UE */

    puts_("\r\nnucleo_reset ready\r\n");

    for (;;) {
        uint32_t isr = U_ISR;
        if (isr & (1u << 3))
            U_ICR = 1u << 3; /* clear overrun */
        if (!(isr & (1u << 5)))
            continue;
        char c = (char)U_RDR;
        if (c == 'r') {
            nrst_assert();
            delay_ms(200);
            nrst_release();
            puts_("RESET\r\n");
        } else if (c == '?') {
            puts_("OK\r\n");
        }
    }
}

extern uint32_t _estack;

void Reset_Handler(void)
{
    main();
}

__attribute__((section(".isr_vector"), used))
const void *vectors[] = {
    &_estack,
    (void *)Reset_Handler,
};
