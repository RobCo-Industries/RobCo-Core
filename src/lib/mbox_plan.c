/*
 * Cache plan for a property mailbox call with the D-cache on.
 * Pure: src/drv/mbox.c applies the plan.
 *
 * The GPU reads and writes the message in memory, past the ARM caches.
 * Cache maintenance must never touch a line that holds other data:
 * a clean after the answer would write the CPU's copy of that line over
 * the GPU's reply. So the message either fills whole lines on its own
 * (direct) or goes through the driver's bounce buffer (bounce).
 */
#include <stdint.h>

#include <robco/mbox.h>

int mbox_plan(uintptr_t buf, uint32_t len, uint32_t line,
              uintptr_t bounce, uint32_t bounce_size, mbox_plan_t *plan)
{
    uint32_t mask = line - 1;

    plan->mode = MBOX_PLAN_REJECT;
    plan->start = 0;
    plan->len = 0;
    if (len == 0 || line == 0 || (line & mask) != 0) {
        return plan->mode;
    }
    if ((buf & mask) == 0 && (len & mask) == 0 && buf + len - 1 >= buf) {
        plan->mode = MBOX_PLAN_DIRECT;
        plan->start = buf;
        plan->len = len;
    } else if (len <= bounce_size && (bounce & mask) == 0 && (bounce_size & mask) == 0) {
        plan->mode = MBOX_PLAN_BOUNCE;
        plan->start = bounce;
        plan->len = (len + mask) & ~mask;
    }
    return plan->mode;
}
