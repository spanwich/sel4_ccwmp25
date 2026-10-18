/* lwIP port for the bare seL4 rootserver. */
#pragma once

#include <stdint.h>
#include <stdlib.h>

#define LWIP_NO_INTTYPES_H 0
#define BYTE_ORDER LITTLE_ENDIAN
void rs_lwip_diag(const char *s);
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { rs_lwip_diag(x); abort(); } while (0)
