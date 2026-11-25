/*
 * Sealing and packing of rimg images. See docs/spec/IMAGE_FORMAT.md,
 * "Sealing and packing". Kept apart from rimg.c so that a program that
 * only validates links no sealing code. Touches no hardware.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/klibc.h>
#include <robco/rimg.h>

#define HDR(field)        (RIMG_HDR_OFFSET + offsetof(rimg_header_t, field))
#define HDR_END           (RIMG_HDR_OFFSET + RIMG_HDR_SIZE)
#define META_END          (HDR_END + RIMG_META_SIZE)
#define TITLE_SIZE        64
#define ICON_SIZE         128
#define ICON_SIDE         32
#define PBM_MAX_DIGITS    9

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* The 8 magic bytes of a known magic, or NULL. */
static const char *magic_bytes(const char *magic)
{
    static const char *const names[] = { "BOOT", "ROM1", "ROBCO78" };
    static const char *const bytes[] = { RIMG_MAGIC_BOOT, RIMG_MAGIC_ROM1, RIMG_MAGIC_ROBCO78 };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (k_strcmp(magic, names[i]) == 0) {
            return bytes[i];
        }
    }
    return NULL;
}

static int known_magic(const uint8_t *m)
{
    return memcmp(m, RIMG_MAGIC_BOOT, 8) == 0 || memcmp(m, RIMG_MAGIC_ROM1, 8) == 0 ||
           memcmp(m, RIMG_MAGIC_ROBCO78, 8) == 0;
}

/* Whitespace in a PBM header. */
static int pbm_hdr_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Reads the three header tokens. Sets the token bounds and *end, the offset
 * after the height. Returns RIMG_OK or RIMG_E_IO for a missing token.
 */
static int pbm_tokens(const uint8_t *d, size_t n, size_t start[3], size_t stop[3], size_t *end)
{
    size_t i = 0;
    int t;

    for (t = 0; t < 3; t++) {
        while (i < n && (pbm_hdr_space(d[i]) || d[i] == '#')) {
            if (d[i] == '#') {
                while (i < n && d[i] != '\r' && d[i] != '\n') {
                    i++;
                }
            } else {
                i++;
            }
        }
        start[t] = i;
        while (i < n && !pbm_hdr_space(d[i]) && d[i] != '#') {
            i++;
        }
        if (start[t] == i) {
            return RIMG_E_IO;
        }
        stop[t] = i;
    }
    *end = i;
    return RIMG_OK;
}

/* A width or height: 1 to 9 ASCII digits. Returns 0 and sets *v, or -1. */
static int pbm_number(const uint8_t *d, size_t start, size_t stop, uint32_t *v)
{
    uint32_t x = 0;
    size_t i;

    if (stop - start > PBM_MAX_DIGITS) {
        return -1;
    }
    for (i = start; i < stop; i++) {
        if (d[i] < '0' || d[i] > '9') {
            return -1;
        }
        x = x * 10 + (uint32_t)(d[i] - '0');
    }
    *v = x;
    return 0;
}

static int pbm_decode(const uint8_t *d, size_t n, uint8_t icon[ICON_SIZE])
{
    size_t start[3], stop[3], end, i, bits = 0;
    uint32_t w, h;
    int p4, rc;

    if (n < 2 || d[0] != 'P' || (d[1] != '1' && d[1] != '4')) {
        return RIMG_E_IO;
    }
    rc = pbm_tokens(d, n, start, stop, &end);
    if (rc != RIMG_OK) {
        return rc;
    }
    if (pbm_number(d, start[1], stop[1], &w) != 0 || pbm_number(d, start[2], stop[2], &h) != 0) {
        return RIMG_E_IO;
    }
    if (w != ICON_SIDE || h != ICON_SIDE) {
        return RIMG_E_SIZE;
    }
    /* The first token is the magic: exactly "P4" reads as P4, else P1, as rimg.py. */
    p4 = stop[0] - start[0] == 2 && d[start[0] + 1] == '4';
    if (p4) {
        if (end >= n || !pbm_hdr_space(d[end])) {
            return RIMG_E_IO;
        }
        if (n - end - 1 != ICON_SIZE) {
            return RIMG_E_SIZE;
        }
        memcpy(icon, d + end + 1, ICON_SIZE);
        return RIMG_OK;
    }
    memset(icon, 0, ICON_SIZE);
    for (i = end; i < n; i++) {
        uint8_t c = d[i];

        if (c == '#') {
            while (i < n && d[i] != '\r' && d[i] != '\n') {
                i++;
            }
            if (i == n) {
                break;
            }
            continue;
        }
        if (c == '0' || c == '1') {
            if (c == '1' && bits < ICON_SIDE * ICON_SIDE) {
                icon[bits / 8] |= (uint8_t)(0x80u >> (bits % 8));
            }
            bits++;
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f') {
            return RIMG_E_IO;
        }
    }
    return bits == ICON_SIDE * ICON_SIDE ? RIMG_OK : RIMG_E_SIZE;
}

/* Icon decoding, before any step. */
static int icon_decode(const uint8_t *d, size_t n, rimg_icon_format_t fmt, uint8_t icon[ICON_SIZE])
{
    if (fmt != RIMG_ICON_AUTO && fmt != RIMG_ICON_RAW && fmt != RIMG_ICON_PBM) {
        return RIMG_E_IO;
    }
    if (fmt == RIMG_ICON_RAW || (fmt == RIMG_ICON_AUTO && n == ICON_SIZE)) {
        if (n != ICON_SIZE) {
            return RIMG_E_SIZE;
        }
        memcpy(icon, d, ICON_SIZE);
        return RIMG_OK;
    }
    if (fmt == RIMG_ICON_PBM && n == ICON_SIZE) {
        return RIMG_E_IO;
    }
    return pbm_decode(d, n, icon);
}

/* ASCII text of at most max bytes. Returns its length, or -1. */
static long ascii_len(const char *s, size_t max)
{
    size_t n = 0;

    while (s[n] != '\0') {
        if ((uint8_t)s[n] >= 0x80 || n == max) {
            return -1;
        }
        n++;
    }
    return (long)n;
}

/* Seal steps 1 to 12 on out, which already holds the len image bytes. */
static int seal_steps(uint8_t *out, size_t len, const char *name, int has_version,
                      uint32_t image_version, const char *title, const uint8_t *icon)
{
    uint32_t file_size;
    long n;

    /* 1. */
    if (len < HDR_END) {
        return RIMG_E_IO;
    }
    /* 2. Bits 27 to 24 of word 0: an ARM b with any condition. */
    if (((get32(out) >> 24) & 0x0F) != 0x0A) {
        return RIMG_E_MAGIC;
    }
    /* 3. */
    if (!known_magic(out + HDR(magic))) {
        return RIMG_E_MAGIC;
    }
    /* 4. */
    if (get16(out + HDR(hdr_version)) != RIMG_HDR_VERSION ||
        get16(out + HDR(hdr_size)) != RIMG_HDR_SIZE) {
        return RIMG_E_VERSION;
    }
    /* 5. A length above 32 bits fails. Two shifts keep a 32-bit size_t free of warnings. */
    file_size = get32(out + HDR(file_size));
    if (((len >> 16) >> 16) != 0 || (file_size != 0 && file_size != len)) {
        return RIMG_E_SIZE;
    }
    put32(out + HDR(file_size), (uint32_t)len);
    /* 6. */
    if (name != NULL) {
        n = ascii_len(name, 16);
        if (n < 0) {
            return RIMG_E_VERSION;
        }
        memset(out + HDR(name), 0, 16);
        memcpy(out + HDR(name), name, (size_t)n);
    }
    /* 7. */
    if (has_version) {
        put32(out + HDR(image_version), image_version);
    }
    if (title != NULL || icon != NULL) {
        /* 8. */
        if ((get32(out + HDR(flags)) & RIMG_FLAG_META) == 0) {
            return RIMG_E_VERSION;
        }
        /* 9. */
        if (len < META_END) {
            return RIMG_E_SIZE;
        }
        /* 10. */
        if (title != NULL) {
            n = ascii_len(title, TITLE_SIZE);
            if (n < 0) {
                return RIMG_E_VERSION;
            }
            memset(out + HDR_END, 0, TITLE_SIZE);
            memcpy(out + HDR_END, title, (size_t)n);
        }
        if (icon != NULL) {
            memcpy(out + HDR_END + TITLE_SIZE, icon, ICON_SIZE);
        }
    }
    /* 11. */
    put32(out + HDR(crc32), rimg_crc(out, (uint32_t)len));
    /* 12. Default expectations: any magic, the subtype's slot, the magic's ABI. */
    return rimg_validate(out, len, NULL, NULL);
}

int rimg_seal(const void *image, size_t len, const rimg_seal_opts_t *opts, void *out)
{
    uint8_t icon[ICON_SIZE];
    int rc;

    if (image == NULL || opts == NULL || out == NULL) {
        return RIMG_E_IO;
    }
    if (opts->icon != NULL) {
        rc = icon_decode(opts->icon, opts->icon_len, opts->icon_format, icon);
        if (rc != RIMG_OK) {
            return rc;
        }
    }
    if (len < HDR_END) {
        return RIMG_E_IO;
    }
    memcpy(out, image, len);
    return seal_steps(out, len, opts->name, opts->has_image_version, opts->image_version,
                      opts->title, opts->icon != NULL ? icon : NULL);
}

int rimg_pack(const void *payload, size_t payload_len, const rimg_pack_opts_t *opts,
              void *out, size_t out_cap, size_t *out_len)
{
    const rimg_seal_opts_t *s;
    const char *magic;
    uint8_t icon[ICON_SIZE];
    uint8_t *o = out;
    size_t code, size;
    uint32_t abi, target;
    int meta, rc;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if (opts == NULL || out == NULL || out_len == NULL || (payload == NULL && payload_len != 0) ||
        opts->meta < -1 || opts->meta > 1) {
        return RIMG_E_IO;
    }
    s = &opts->seal;
    if (s->icon != NULL) {
        rc = icon_decode(s->icon, s->icon_len, s->icon_format, icon);
        if (rc != RIMG_OK) {
            return rc;
        }
    }
    /* 1. */
    magic = opts->magic == NULL ? NULL : magic_bytes(opts->magic);
    if (magic == NULL) {
        return RIMG_E_MAGIC;
    }
    /* 2. */
    if (opts->subtype < 0 || opts->subtype > 0xFFFFFFFF || opts->load_addr < 0 ||
        opts->load_addr > 0xFFFFFFFF ||
        (opts->has_mem_size && (opts->mem_size < 0 || opts->mem_size > 0xFFFFFFFF))) {
        return RIMG_E_BOUNDS;
    }
    /* 3. */
    meta = opts->meta;
    if (meta < 0) {
        meta = memcmp(magic, RIMG_MAGIC_BOOT, 8) != 0;
    }
    if (!meta && (s->title != NULL || s->icon != NULL)) {
        return RIMG_E_VERSION;
    }
    /* 4. */
    if (opts->entry < 0 || opts->entry % 4 != 0 || (uint64_t)opts->entry + 4 > payload_len) {
        return RIMG_E_SIZE;
    }
    /* 5. Compare before adding, so the sum cannot wrap. */
    code = meta ? META_END : HDR_END;
    if ((uint64_t)payload_len > 0xFFFFFFFFu - (uint64_t)code) {
        return RIMG_E_SIZE;
    }
    size = code + payload_len;
    if (out_cap < size) {
        return RIMG_E_IO;
    }
    /* 6. A b reaches 32 MiB forward from PC + 8. entry < payload_len, so no overflow. */
    target = (uint32_t)(code + (size_t)opts->entry);
    if (((target - 8) >> 2) >= (1u << 23)) {
        return RIMG_E_VERSION;
    }
    memset(o, 0, size);
    put32(o, 0xEA000000u | (((target - 8) >> 2) & 0x00FFFFFFu));
    /* 7. */
    abi = opts->has_abi_version ? opts->abi_version
                                : (memcmp(magic, RIMG_MAGIC_BOOT, 8) == 0 ? 2u : 1u) << 16;
    memcpy(o + HDR(magic), magic, 8);
    put16(o + HDR(hdr_version), RIMG_HDR_VERSION);
    put16(o + HDR(hdr_size), RIMG_HDR_SIZE);
    put32(o + HDR(flags), meta ? RIMG_FLAG_META : 0);
    put32(o + HDR(load_addr), (uint32_t)opts->load_addr);
    put32(o + HDR(file_size), (uint32_t)size);
    put32(o + HDR(mem_size), opts->has_mem_size ? (uint32_t)opts->mem_size : (uint32_t)size);
    put32(o + HDR(abi_version), abi);
    put32(o + HDR(image_version), s->has_image_version ? s->image_version : 0);
    put32(o + HDR(subtype), (uint32_t)opts->subtype);
    /* 8. */
    if (payload_len != 0) {
        memcpy(o + code, payload, payload_len);
    }
    /* 9. */
    rc = seal_steps(o, size, s->name, 0, 0, s->title, s->icon != NULL ? icon : NULL);
    if (rc == RIMG_OK) {
        *out_len = size;
    }
    return rc;
}
