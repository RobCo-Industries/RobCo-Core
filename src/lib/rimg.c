#include <stddef.h>
#include <stdint.h>

#include <robco/crc32.h>
#include <robco/klibc.h>
#include <robco/rimg.h>

#define RIMG_CRC_OFFSET (RIMG_HDR_OFFSET + 0x1c)
#define RIMG_HDR_END    (RIMG_HDR_OFFSET + RIMG_HDR_SIZE)

enum { KIND_BOOT, KIND_ROM1, KIND_HOLO, KIND_COUNT };

static const char *const magics[KIND_COUNT] = {
    RIMG_MAGIC_BOOT, RIMG_MAGIC_ROM1, RIMG_MAGIC_ROBCO78,
};
static const uint32_t abi_majors[KIND_COUNT] = { 2, 1, 1 };

typedef struct {
    uint8_t kind;
    uint8_t subtype;
    uint32_t base, size;
} slot_t;

/* Subtypes with a slot. Others are reserved or not executable. */
static const slot_t slots[] = {
    { KIND_BOOT, RIMG_BOOT_UOS, 0x00400000, 0x00c00000 },
    { KIND_BOOT, RIMG_BOOT_MAINT, 0x00400000, 0x00c00000 },
    { KIND_BOOT, RIMG_BOOT_DIAG, 0x00400000, 0x00c00000 },
    { KIND_BOOT, RIMG_BOOT_AGENT, 0x00200000, 0x00200000 },
    { KIND_BOOT, RIMG_BOOT_BIOS, 0x00008000, 0x000f8000 },
    { KIND_BOOT, RIMG_BOOT_PIPOS, 0x00008000, 0x003f8000 },
    { KIND_BOOT, RIMG_BOOT_EXAMPLE, 0x00008000, 0x000f8000 },
    { KIND_ROM1, RIMG_ROM1_SYSTEM, 0x00400000, 0x00400000 },
    { KIND_HOLO, RIMG_HOLO_GAME, 0x00800000, 0x00400000 },
    { KIND_HOLO, RIMG_HOLO_UTILITY, 0x00800000, 0x00400000 },
};

static const char *const err_names[] = {
    "ok", "nofile", "io", "magic", "version", "size", "bounds", "abi", "crc",
};

const char *rimg_strerror(int code)
{
    if (code < 0 || code >= (int)(sizeof(err_names) / sizeof(err_names[0]))) {
        return "unknown";
    }
    return err_names[code];
}

static int magic_kind(const char magic[8])
{
    int k;

    for (k = 0; k < KIND_COUNT; k++) {
        if (memcmp(magic, magics[k], 8) == 0) {
            return k;
        }
    }
    return -1;
}

static const slot_t *find_slot(int kind, uint32_t subtype)
{
    size_t i;

    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        if (slots[i].kind == kind && slots[i].subtype == subtype) {
            return &slots[i];
        }
    }
    return NULL;
}

/* Steps 1 to 5 on the header at bytes[0x20..0x60). Sets *hdr on success. */
static int check_header(const uint8_t *bytes, size_t avail, const rimg_expect_t *expect,
                        const rimg_header_t **hdr)
{
    const rimg_header_t *h = (const rimg_header_t *)(bytes + RIMG_HDR_OFFSET);
    const slot_t *slot;
    uint32_t base, size, min_size, want_abi;
    int kind;

    /* 1. Magic. */
    kind = magic_kind(h->magic);
    if (kind < 0 || (expect != NULL && expect->magic != NULL &&
                     memcmp(h->magic, expect->magic, 8) != 0)) {
        return RIMG_E_MAGIC;
    }

    /* 2. Header version, size, flags and reserved. */
    if (h->hdr_version != RIMG_HDR_VERSION || h->hdr_size != RIMG_HDR_SIZE ||
        (h->flags & ~(uint32_t)RIMG_FLAG_META) != 0 || h->reserved != 0) {
        return RIMG_E_VERSION;
    }

    /* 3. File size. */
    min_size = RIMG_HDR_END + ((h->flags & RIMG_FLAG_META) ? RIMG_META_SIZE : 0);
    if (h->file_size < min_size || h->file_size > avail ||
        (expect != NULL && expect->file_size != 0 && h->file_size != expect->file_size)) {
        return RIMG_E_SIZE;
    }

    /* 4. Subtype, slot and memory bounds. */
    slot = find_slot(kind, h->subtype);
    if (slot == NULL) {
        return RIMG_E_BOUNDS;
    }
    base = slot->base;
    size = slot->size;
    if (expect != NULL && expect->slot_size != 0) {
        base = expect->slot_base;
        size = expect->slot_size;
    }
    if (h->load_addr != base || h->mem_size < h->file_size ||
        (uint64_t)h->load_addr + h->mem_size > (uint64_t)base + size) {
        return RIMG_E_BOUNDS;
    }

    /* 5. ABI major. The minor is not checked. */
    want_abi = abi_majors[kind];
    if (expect != NULL && expect->abi_major != 0) {
        want_abi = expect->abi_major;
    }
    if ((h->abi_version >> 16) != want_abi) {
        return RIMG_E_ABI;
    }
    *hdr = h;
    return RIMG_OK;
}

int rimg_check_header(const void *head, size_t avail, const rimg_expect_t *expect)
{
    const rimg_header_t *h;

    if (head == NULL || avail < RIMG_HDR_END) {
        return RIMG_E_IO;
    }
    return check_header(head, avail, expect, &h);
}

int rimg_validate(const void *image, size_t avail, const rimg_expect_t *expect,
                  const rimg_header_t **out)
{
    static const uint8_t zero[4];
    const uint8_t *bytes = image;
    const rimg_header_t *h;
    uint32_t crc;
    int rc;

    if (image == NULL || avail < RIMG_HDR_END) {
        return RIMG_E_IO;
    }
    rc = check_header(bytes, avail, expect, &h);
    if (rc != RIMG_OK) {
        return rc;
    }

    /* 6. CRC over [0, file_size) with the crc32 field read as zero. */
    crc = crc32_update(0, bytes, RIMG_CRC_OFFSET);
    crc = crc32_update(crc, zero, sizeof(zero));
    crc = crc32_update(crc, bytes + RIMG_CRC_OFFSET + 4, h->file_size - RIMG_CRC_OFFSET - 4);
    if (crc != h->crc32) {
        return RIMG_E_CRC;
    }

    if (out != NULL) {
        *out = h;
    }
    return RIMG_OK;
}
