#ifndef ROBCO_DRV_MBOX_CACHE_H
#define ROBCO_DRV_MBOX_CACHE_H

#include <stdint.h>

#include <robco/mbox.h>

/*
 * Cache side of mbox_call(), in src/drv/mbox_cache.c. mmu_enable()
 * references it, so only images that can turn the D-cache on link it.
 */

/*
 * Before the request. Returns the buffer to send: `buf`, or the bounce
 * buffer holding a copy. NULL when the message can not be sent safely.
 */
uint32_t *mbox_cache_begin(uint32_t *buf, uint32_t size, mbox_plan_t *plan);

/* After the answer. Makes the reply visible in `buf`. */
void mbox_cache_end(uint32_t *buf, const uint32_t *sent, uint32_t size,
                    const mbox_plan_t *plan);

#endif
