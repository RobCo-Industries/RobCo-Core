#include <stddef.h>
#include <stdint.h>

#include <robco/alloc.h>
#include <robco/board.h>

/* From ld/image.ld.S. __slot_end is 0 when ROBCO_SLOT_END is not set. */
extern char __heap_start[];
extern char __slot_end[];

size_t robco_heap_slot(void **base)
{
    uintptr_t start = (uintptr_t)__heap_start;
    uintptr_t end = (uintptr_t)__slot_end;

    *base = __heap_start;
    /* Also covers an unset slot end, which is 0. */
    if (end <= start) {
        return 0;
    }
    return end - start;
}

size_t robco_heap_high(void **base)
{
    const robco_board_t *b = board_get();
    uint32_t top = b->mem_base + b->mem_size;

    *base = (void *)(uintptr_t)ROBCO_HEAP_HIGH_BASE;
    if (b->mem_size == 0 || top <= ROBCO_HEAP_HIGH_BASE) {
        return 0;
    }
    return top - ROBCO_HEAP_HIGH_BASE;
}
