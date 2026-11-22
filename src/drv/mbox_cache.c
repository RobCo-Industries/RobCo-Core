/*
 * The GPU reads and writes the message in memory, past the ARM caches.
 * With the D-cache on, maintenance only touches lines that hold the
 * message alone: the caller's buffer when it fills whole 64-byte lines,
 * else the bounce buffer. Clean them before the request, so the GPU sees
 * the message. Invalidate them after the answer, so the CPU reads the
 * GPU's data, not lines it fetched before or during the GPU write. No
 * other data shares these lines, so dropping them loses nothing.
 */
#include <stdint.h>

#include <robco/cache.h>
#include <robco/klibc.h>
#include <robco/mbox.h>

#include "mbox_cache.h"

_Static_assert(ROBCO_MBOX_BOUNCE_SIZE > 0 && ROBCO_MBOX_BOUNCE_SIZE % CACHE_LINE_MAX == 0,
               "ROBCO_MBOX_BOUNCE_SIZE must be a non-zero multiple of 64");

/* Aligned to the largest line and sized in whole lines: no other object shares them. */
static uint32_t bounce[ROBCO_MBOX_BOUNCE_SIZE / 4] __attribute__((aligned(CACHE_LINE_MAX)));

static int dcache_on(void)
{
    uint32_t sctlr;

    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    return (sctlr & (1u << 2)) != 0;
}

uint32_t *mbox_cache_begin(uint32_t *buf, uint32_t size, mbox_plan_t *plan)
{
    if (!dcache_on()) {
        plan->mode = MBOX_PLAN_DIRECT;
        plan->start = 0;
        plan->len = 0;
        return buf;
    }
    if (mbox_plan((uintptr_t)buf, size, CACHE_LINE_MAX, (uintptr_t)bounce,
                  sizeof(bounce), plan) == MBOX_PLAN_REJECT) {
        return 0;
    }
    if (plan->mode == MBOX_PLAN_BOUNCE) {
        memcpy(bounce, buf, size);
        buf = bounce;
    }
    cache_clean_range((const void *)plan->start, plan->len);
    return buf;
}

void mbox_cache_end(uint32_t *buf, const uint32_t *sent, uint32_t size,
                    const mbox_plan_t *plan)
{
    if (plan->len == 0) {
        return;
    }
    cache_invalidate_range((const void *)plan->start, plan->len);
    if (sent != buf) {
        memcpy(buf, sent, size);
    }
}
