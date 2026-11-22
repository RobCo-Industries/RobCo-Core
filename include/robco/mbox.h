#ifndef ROBCO_MBOX_H
#define ROBCO_MBOX_H

#include <stdint.h>

#define MBOX_TAG_BOARD_REV   0x00010002
#define MBOX_TAG_ARM_MEMORY  0x00010005
#define MBOX_TAG_CLOCK_RATE  0x00030002

#define MBOX_CLOCK_UART      2

/*
 * Bytes in the bounce buffer that mbox_call() uses with the D-cache on.
 * A multiple of 64. The largest library message, from fb_init(), is
 * 136 bytes.
 */
#ifndef ROBCO_MBOX_BOUNCE_SIZE
#define ROBCO_MBOX_BOUNCE_SIZE  256
#endif

#define MBOX_ERR        (-1)    /* bad board or buffer, timeout, error answer */
#define MBOX_ERR_SIZE   (-2)    /* D-cache on, message too large for the bounce */

/*
 * Sends a property buffer on channel 8 and waits for the answer.
 * `buf` must be 16-byte aligned; buf[0] holds its size in bytes.
 * Returns 0 when the firmware reports success, else MBOX_ERR or
 * MBOX_ERR_SIZE.
 *
 * With the D-cache on, cache maintenance never touches data next to the
 * message. A message that starts on a 64-byte boundary and fills whole
 * 64-byte lines is used in place. Any other message is copied through a
 * driver-owned bounce buffer of ROBCO_MBOX_BOUNCE_SIZE bytes. A larger
 * message that is not line-isolated fails with MBOX_ERR_SIZE. With the
 * D-cache off, `buf` is always used in place.
 *
 * Not reentrant: do not call it, or the helpers below, from IRQ context.
 */
int mbox_call(uint32_t *buf);

/* Each returns 0 on success, -1 on failure. */
int mbox_get_board_rev(uint32_t *rev);
int mbox_get_arm_memory(uint32_t *base, uint32_t *size);
int mbox_get_clock_rate(uint32_t clock_id, uint32_t *hz);

#define MBOX_PLAN_REJECT    0
#define MBOX_PLAN_BOUNCE    1
#define MBOX_PLAN_DIRECT    2

/* Clean [start, start + len) before the call, invalidate it after. */
typedef struct {
    int mode;           /* MBOX_PLAN_* */
    uintptr_t start;
    uint32_t len;
} mbox_plan_t;

/*
 * Picks how mbox_call() handles a `len`-byte message at `buf` with the
 * D-cache on. `line` is a power of two. DIRECT when `buf` and `len` are
 * multiples of `line`. Else BOUNCE when `len` fits the bounce buffer at
 * `bounce` (line-aligned, `bounce_size` a multiple of `line`); the range
 * then covers the bounce buffer, rounded up to whole lines. Else, or for
 * a `len` of 0, REJECT. Returns the mode.
 */
int mbox_plan(uintptr_t buf, uint32_t len, uint32_t line,
              uintptr_t bounce, uint32_t bounce_size, mbox_plan_t *plan);

#endif
