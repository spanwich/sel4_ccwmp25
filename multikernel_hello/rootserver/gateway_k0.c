/*
 * K0 side of the paper experiments: the access-control engine (none |
 * EverParse | Cedar, selected at build time by MK_ENGINE) and the in-image
 * equivalence run (C1, the paper's test_equivalence.c, 12,527 comparisons).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <string.h>

#include "rs.h"
#include "gateway_k0.h"

#if KERNEL_ID == 0

#ifndef MK_ENGINE_none
#include "gateway_engine.h"
int equivalence_main(void);
#endif

const char *gw_engine_label(void)
{
    return MK_ENGINE_NAME;
}

/* C1: returns 0 on EQUIVALENCE_OK. Engine none has nothing to compare. */
int gw_equivalence(void)
{
#ifdef MK_ENGINE_none
    put_str("c1: engine none, skipped\n");
    return 0;
#else
    uint64_t t0 = rs_cntpct();
    put_str("c1: start engine=" MK_ENGINE_NAME "\n");
    int rc = equivalence_main();
    fflush(stdout);
    fflush(stderr);
    put_str("c1: done rc=");
    put_dec((uint64_t)rc);
    put_str(" ms=");
    put_dec((rs_cntpct() - t0) * 1000u / rs_cntfrq());
    put_str("\n");
    return rc;
#endif
}

#endif /* KERNEL_ID == 0 */
