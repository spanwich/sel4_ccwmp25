/* lwIP options for the K1 rootserver (bare seL4, single thread, raw API). */
#pragma once

#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_ARP                1
#define LWIP_ICMP               1
#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_DHCP               0
#define LWIP_AUTOIP             0
#define LWIP_IGMP               0
#define LWIP_DNS                0
#define LWIP_RAW                0
#define LWIP_STATS              0
#define LWIP_NETIF_LINK_CALLBACK 0

#define MEM_ALIGNMENT           8
#define MEM_SIZE                (256 * 1024)
#define MEMP_NUM_PBUF           64
#define MEMP_NUM_TCP_PCB        16
#define MEMP_NUM_TCP_SEG        64
#define PBUF_POOL_SIZE          128
#define PBUF_POOL_BUFSIZE       1536
#define TCP_MSS                 1460
#define TCP_WND                 (8 * TCP_MSS)
#define TCP_SND_BUF             (8 * TCP_MSS)
#define TCP_SND_QUEUELEN        32
#define ETH_PAD_SIZE            0
#define LWIP_CHKSUM_ALGORITHM   3
#define LWIP_RAND()             ((u32_t)lwip_rand())
unsigned int lwip_rand(void);
