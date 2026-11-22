/*
 * Pointer checks for system-call handlers. Pure logic: no hardware.
 * Every comparison uses offsets from a region start, so no sum can
 * wrap past the top of the address space.
 */

#include <robco/umode.h>

/* Region holding `p` with the right permission, or 0. */
static const umode_region_t *find(const umode_region_t *r, unsigned n, uintptr_t p, int write)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (r[i].size == 0 || p < r[i].start || p - r[i].start >= r[i].size) {
            continue;
        }
        if (write && !(r[i].flags & UMODE_REGION_WRITE)) {
            continue;
        }
        return &r[i];
    }
    return 0;
}

/* Zero-length: ptr inside a region, or exactly at its end. */
static int edge_ok(const umode_region_t *r, unsigned n, uintptr_t p, int write)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (r[i].size == 0 || p < r[i].start || p - r[i].start > r[i].size) {
            continue;
        }
        if (write && !(r[i].flags & UMODE_REGION_WRITE)) {
            continue;
        }
        return 1;
    }
    return 0;
}

int umode_regions_range_ok(const umode_region_t *regions, unsigned count,
                           uintptr_t ptr, size_t len, int write)
{
    const umode_region_t *r;
    size_t room;
    unsigned steps;

    if (regions == 0) {
        return 0;
    }
    if (len == 0) {
        return edge_ok(regions, count, ptr, write);
    }
    /* The buffer may cross into an adjacent region: at most `count` hops. */
    for (steps = 0; steps < count; steps++) {
        r = find(regions, count, ptr, write);
        if (r == 0) {
            return 0;
        }
        room = r->size - (size_t)(ptr - r->start);  /* bytes left in r, >= 1 */
        if (len <= room) {
            return 1;
        }
        len -= room;
        ptr += room;
        if (ptr == 0) {
            return 0;   /* wrapped past the top of the address space */
        }
    }
    return 0;
}

long umode_regions_strnlen(const umode_region_t *regions, unsigned count,
                           uintptr_t ptr, size_t max)
{
    const umode_region_t *r;
    const volatile char *p;
    size_t done = 0, room, i;

    if (regions == 0) {
        return -1;
    }
    while (done < max) {
        r = find(regions, count, ptr, 0);
        if (r == 0) {
            return -1;
        }
        room = r->size - (size_t)(ptr - r->start);
        if (room > max - done) {
            room = max - done;
        }
        p = (const volatile char *)ptr;
        for (i = 0; i < room; i++) {
            if (p[i] == 0) {
                return (long)(done + i);
            }
        }
        done += room;
        ptr += room;
        if (ptr == 0) {
            return -1;
        }
    }
    return -1;
}
