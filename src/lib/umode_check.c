/*
 * Task validation and pointer checks for system-call handlers. Pure
 * logic: no hardware. Every comparison uses offsets from a region start
 * or inclusive last bytes, so no sum can wrap past the top of the
 * address space, on a 32-bit target or a 64-bit host.
 */

#include <stdint.h>

#include <robco/umode.h>

/* Region is usable: non-empty and its last byte does not wrap. Others are ignored. */
static int usable(const umode_region_t *r)
{
    return r->size != 0 && r->size - 1 <= UINTPTR_MAX - r->start;
}

/* Region holding `p` with the right permission, or 0. */
static const umode_region_t *find(const umode_region_t *r, unsigned n, uintptr_t p, int write)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (!usable(&r[i]) || p < r[i].start || p - r[i].start >= r[i].size) {
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
        if (!usable(&r[i]) || p < r[i].start || p - r[i].start > r[i].size) {
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
        /* Bytes left in r, >= 1. r does not wrap, so ptr + room <= UINTPTR_MAX + 1. */
        room = r->size - (size_t)(ptr - r->start);
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

/* Last byte of [start, start + size), or 0 with *ok = 0 when it wraps or size is 0. */
static uintptr_t last_byte(uintptr_t start, size_t size, int *ok)
{
    *ok = size != 0 && size - 1 <= UINTPTR_MAX - start;
    return *ok ? start + (size - 1) : 0;
}

/* Two closed byte ranges [a, al] and [b, bl] share a byte. */
static int meet(uintptr_t a, uintptr_t al, uintptr_t b, uintptr_t bl)
{
    return a <= bl && b <= al;
}

static int check_regions(const umode_region_t *r, unsigned n, const umode_limits_t *lim)
{
    uintptr_t last[UMODE_MAX_REGIONS];
    size_t sec = lim->section;
    unsigned i, j;
    int ok;

    for (i = 0; i < n; i++) {
        if ((r[i].flags & ~UMODE_REGION_WRITE) != 0) {
            return UMODE_ERR_ARG;
        }
        last[i] = last_byte(r[i].start, r[i].size, &ok);
        if (!ok) {
            return UMODE_ERR_RANGE;
        }
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (meet(r[i].start, last[i], r[j].start, last[j])) {
                return UMODE_ERR_OVERLAP;
            }
            if (sec != 0 && meet(r[i].start / sec, last[i] / sec, r[j].start / sec, last[j] / sec)) {
                return UMODE_ERR_SECTION;
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (sec != 0 && (r[i].start % sec != 0 || r[i].size % sec != 0)) {
            return UMODE_ERR_ALIGN;
        }
        for (j = 0; j < lim->forbidden_count; j++) {
            const umode_forbidden_t *f = &lim->forbidden[j];
            uintptr_t fl;

            if (f->size == 0) {
                continue;
            }
            fl = last_byte(f->start, f->size, &ok);
            if (!ok) {
                fl = UINTPTR_MAX;   /* runs to the top: clamp */
            }
            if (meet(r[i].start, last[i], f->start, fl)) {
                return f->err;
            }
        }
        if (lim->ram_size == 0 || r[i].start < lim->ram_start ||
            r[i].start - lim->ram_start >= lim->ram_size ||
            r[i].size > lim->ram_size - (r[i].start - lim->ram_start)) {
            return UMODE_ERR_RAM;
        }
    }
    return 0;
}

int umode_task_check(const umode_task_t *t, const umode_limits_t *lim)
{
    const umode_region_t *r;
    unsigned n;
    int rc, ok;

    if (t == 0 || lim == 0 || t->regions == 0 || t->region_count == 0 ||
        t->region_count > UMODE_MAX_REGIONS ||
        (lim->forbidden == 0 && lim->forbidden_count != 0)) {
        return UMODE_ERR_ARG;
    }
    r = t->regions;
    n = t->region_count;
    rc = check_regions(r, n, lim);
    if (rc != 0) {
        return rc;
    }
    if ((t->entry & 3) != 0 || !umode_regions_range_ok(r, n, t->entry, 4, 0)) {
        return UMODE_ERR_ENTRY;
    }
    /* Full-descending stack: the first push writes [sp - 8, sp). */
    if ((t->sp & 7) != 0 || t->sp < 8 || !umode_regions_range_ok(r, n, t->sp - 8, 8, 1)) {
        return UMODE_ERR_SP;
    }
    if ((t->stub & 3) != 0 || !umode_regions_range_ok(r, n, t->stub, UMODE_STUB_SIZE, 1) ||
        meet(t->stub, last_byte(t->stub, UMODE_STUB_SIZE, &ok), t->entry, (uintptr_t)t->entry + 3)) {
        return UMODE_ERR_STUB;
    }
    return 0;
}

long umode_stray_section(const umode_region_t *regions, unsigned count,
                         int (*user)(uint32_t addr))
{
    uintptr_t a, al;
    unsigned i;
    long s;

    for (s = 0; s < 4096; s++) {
        a = (uintptr_t)s << 20;
        al = a + 0xfffffu;
        if (user((uint32_t)a) == 0) {
            continue;
        }
        for (i = 0; i < count; i++) {
            if (usable(&regions[i]) && meet(a, al, regions[i].start,
                                             regions[i].start + (regions[i].size - 1))) {
                break;
            }
        }
        if (i == count) {
            return s;
        }
    }
    return -1;
}
