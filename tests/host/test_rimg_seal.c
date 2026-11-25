/*
 * rimg_seal() and rimg_pack() against tests/vectors/rimg/vectors.ini,
 * read with the library's INI parser. Every vector must give the bytes of
 * its output file, or fail with its code. Runs from the repository root.
 */
#include <robco/ini.h>
#include <robco/rimg.h>

#include "rc_test.h"

#define VEC_DIR     "tests/vectors/rimg/"
#define MAX_VECTORS 128
#define MAX_FILE    65536
#define TEXT_MAX    (INI_MAX_VALUE + 1)

typedef struct {
    char id[INI_MAX_SECTION + 1];
    unsigned have;      /* bit per key, see KEYS */
    char val[16][TEXT_MAX];
} vector_t;

static const char *const KEYS[16] = {
    "op", "input", "magic", "subtype", "load_addr", "mem_size", "entry", "name",
    "image_version", "abi_version", "title", "meta", "icon", "icon_format", "expect", "output",
};
enum { K_OP, K_INPUT, K_MAGIC, K_SUBTYPE, K_LOAD, K_MEM, K_ENTRY, K_NAME, K_IVER, K_ABI,
       K_TITLE, K_META, K_ICON, K_ICON_FMT, K_EXPECT, K_OUTPUT };

static vector_t vecs[MAX_VECTORS];
static int nvecs;
static int bad_key;

static uint8_t in_buf[MAX_FILE], icon_buf[MAX_FILE], want_buf[MAX_FILE], out_buf[MAX_FILE];

static int on_section(const char *section, unsigned line, void *ctx)
{
    (void)line;
    (void)ctx;
    if (strncmp(section, "vector ", 7) != 0 || nvecs == MAX_VECTORS) {
        bad_key = 1;
        return 1;
    }
    snprintf(vecs[nvecs].id, sizeof(vecs[0].id), "%s", section + 7);
    nvecs++;
    return 0;
}

static int on_key(const char *section, const char *key, const char *value, unsigned line,
                  void *ctx)
{
    vector_t *v = &vecs[nvecs - 1];
    int k;

    (void)section;
    (void)ctx;
    for (k = 0; k < 16; k++) {
        if (strcmp(key, KEYS[k]) == 0 && !(v->have & (1u << k))) {
            v->have |= 1u << k;
            snprintf(v->val[k], TEXT_MAX, "%s", value);
            return 0;
        }
    }
    printf("  vectors.ini:%u: unknown or repeated key %s\n", line, key);
    bad_key = 1;
    return 1;
}

static size_t load(const char *name, uint8_t *buf, int *ok)
{
    char path[256];
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), VEC_DIR "%s", name);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("  cannot open %s\n", path);
        *ok = 0;
        return 0;
    }
    n = fread(buf, 1, MAX_FILE, f);
    fclose(f);
    return n;
}

static int read_vectors(void)
{
    static char text[MAX_FILE];
    unsigned line = 0;
    int ok = 1;
    size_t n = load("vectors.ini", (uint8_t *)text, &ok);

    nvecs = 0;
    bad_key = 0;
    memset(vecs, 0, sizeof(vecs));
    if (!ok || ini_parse_sections(text, n, on_key, on_section, NULL, &line) != 0 || bad_key) {
        printf("  vectors.ini does not parse, line %u\n", line);
        return 0;
    }
    return 1;
}

/* A C literal: an optional '-', then decimal or 0x hex. */
static int64_t number(const char *s)
{
    int neg = *s == '-';
    int64_t v;

    if (neg) {
        s++;
    }
    v = (int64_t)strtoull(s, NULL, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
    return neg ? -v : v;
}

/* Parses X.Y.Z or MAJOR.MINOR as the caller of the C functions must. */
static uint32_t version(const char *s, int parts)
{
    unsigned long a = 0, b = 0, c = 0;

    if (parts == 3) {
        sscanf(s, "%lu.%lu.%lu", &a, &b, &c);
        return (uint32_t)((a << 16) | (b << 8) | c);
    }
    sscanf(s, "%lu.%lu", &a, &b);
    return (uint32_t)((a << 16) | b);
}

static rimg_icon_format_t icon_format(const vector_t *v)
{
    if (!(v->have & (1u << K_ICON_FMT)) || strcmp(v->val[K_ICON_FMT], "auto") == 0) {
        return RIMG_ICON_AUTO;
    }
    return strcmp(v->val[K_ICON_FMT], "raw") == 0 ? RIMG_ICON_RAW : RIMG_ICON_PBM;
}

#define HAS(k) ((v->have & (1u << (k))) != 0)

/* Runs one vector. Returns 1 when it gives the expected bytes or code. */
static int run_vector(const vector_t *v)
{
    rimg_seal_opts_t s;
    rimg_pack_opts_t p;
    size_t in_len, want_len = 0, out_len = 0;
    int ok = 1, rc;

    memset(&s, 0, sizeof(s));
    in_len = load(v->val[K_INPUT], in_buf, &ok);
    if (HAS(K_NAME)) {
        s.name = v->val[K_NAME];
    }
    if (HAS(K_TITLE)) {
        s.title = v->val[K_TITLE];
    }
    if (HAS(K_IVER)) {
        s.has_image_version = 1;
        s.image_version = version(v->val[K_IVER], 3);
    }
    if (HAS(K_ICON)) {
        s.icon_len = load(v->val[K_ICON], icon_buf, &ok);
        s.icon = icon_buf;
        s.icon_format = icon_format(v);
    }
    if (HAS(K_OUTPUT)) {
        want_len = load(v->val[K_OUTPUT], want_buf, &ok);
    }
    if (!ok) {
        return 0;
    }
    memset(out_buf, 0xA5, sizeof(out_buf));
    if (strcmp(v->val[K_OP], "seal") == 0) {
        /* An out of exactly in_len bytes, so ASan sees a write past it. */
        uint8_t *exact = malloc(in_len ? in_len : 1);

        if (exact == NULL) {
            return 0;
        }
        rc = rimg_seal(in_buf, in_len, &s, exact);
        memcpy(out_buf, exact, in_len);
        free(exact);
        out_len = rc == RIMG_OK ? in_len : 0;
    } else {
        memset(&p, 0, sizeof(p));
        p.seal = s;
        p.magic = v->val[K_MAGIC];
        p.subtype = number(v->val[K_SUBTYPE]);
        p.load_addr = number(v->val[K_LOAD]);
        p.has_mem_size = HAS(K_MEM);
        p.mem_size = HAS(K_MEM) ? number(v->val[K_MEM]) : 0;
        p.has_abi_version = HAS(K_ABI);
        p.abi_version = HAS(K_ABI) ? version(v->val[K_ABI], 2) : 0;
        p.meta = !HAS(K_META) ? -1 : strcmp(v->val[K_META], "yes") == 0;
        p.entry = HAS(K_ENTRY) ? number(v->val[K_ENTRY]) : 0;
        out_len = 12345;
        rc = rimg_pack(in_buf, in_len, &p, out_buf, sizeof(out_buf), &out_len);
        if (rc != RIMG_OK && out_len != 0) {
            printf("  %s: out_len %zu after a failure\n", v->id, out_len);
            return 0;
        }
    }
    if (strcmp(rimg_strerror(rc), v->val[K_EXPECT]) != 0) {
        printf("  %s: got %s, want %s\n", v->id, rimg_strerror(rc), v->val[K_EXPECT]);
        return 0;
    }
    if (rc == RIMG_OK && (out_len != want_len || memcmp(out_buf, want_buf, want_len) != 0)) {
        printf("  %s: %zu bytes differ from %s\n", v->id, out_len, v->val[K_OUTPUT]);
        return 0;
    }
    return 1;
}

TEST(every_vector)
{
    int i, passed = 0, seal = 0, pack = 0;

    CHECK(read_vectors());
    CHECK(nvecs >= 54);
    for (i = 0; i < nvecs; i++) {
        passed += run_vector(&vecs[i]);
        seal += strcmp(vecs[i].val[K_OP], "seal") == 0;
        pack += strcmp(vecs[i].val[K_OP], "pack") == 0;
    }
    CHECK_EQ(passed, nvecs);
    CHECK_EQ(seal + pack, nvecs);
    CHECK(seal > 0 && pack > 0);
}

/* A sealable holotape image of 0x140 bytes with a META block. */
static size_t holo_image(uint8_t *img)
{
    rimg_pack_opts_t p;
    uint8_t payload[32] = { 0 };
    size_t n = 0;

    memset(&p, 0, sizeof(p));
    p.magic = "ROBCO78";
    p.load_addr = 0x800000;
    p.meta = -1;
    CHECK_EQ(rimg_pack(payload, sizeof(payload), &p, img, 4096, &n), RIMG_OK);
    return n;
}

static int seal_icon(const char *pbm)
{
    static uint8_t img[4096], out[4096];
    rimg_seal_opts_t s;
    size_t n = holo_image(img);

    memset(&s, 0, sizeof(s));
    s.icon = (const uint8_t *)pbm;
    s.icon_len = strlen(pbm);
    s.icon_format = RIMG_ICON_PBM;
    return rimg_seal(img, n, &s, out);
}

/* 1024 zero pixels after a P1 header of the given width and height. */
static const char *p1(const char *w, const char *h)
{
    static char buf[2200];
    int n = snprintf(buf, sizeof(buf), "P1\n%s %s\n", w, h);
    int i;

    for (i = 0; i < 1024; i++) {
        buf[n++] = '0';
    }
    buf[n] = '\0';
    return buf;
}

TEST(pbm_header_numbers)
{
    CHECK_EQ(seal_icon(p1("32", "32")), RIMG_OK);
    CHECK_EQ(seal_icon(p1("032", "000000032")), RIMG_OK);
    CHECK_EQ(seal_icon(p1("+32", "32")), RIMG_E_IO);
    CHECK_EQ(seal_icon(p1("-32", "32")), RIMG_E_IO);
    CHECK_EQ(seal_icon(p1("3_2", "32")), RIMG_E_IO);
    CHECK_EQ(seal_icon(p1("0000000032", "32")), RIMG_E_IO);
    CHECK_EQ(seal_icon(p1("32\v", "32")), RIMG_E_IO);
    CHECK_EQ(seal_icon(p1("32", "32\f")), RIMG_E_IO);
    CHECK_EQ(seal_icon(p1("32", "+32")), RIMG_E_IO);
    CHECK_EQ(seal_icon(p1("0x20", "32")), RIMG_E_IO);
    CHECK_EQ(seal_icon(p1("33", "32")), RIMG_E_SIZE);
    CHECK_EQ(seal_icon(p1("0", "00")), RIMG_E_SIZE);
    CHECK_EQ(seal_icon("P1\n32"), RIMG_E_IO);
    CHECK_EQ(seal_icon("P2\n32 32\n"), RIMG_E_IO);
}

TEST(icon_p4_header_end)
{
    static char pbm[200];
    size_t n = (size_t)snprintf(pbm, sizeof(pbm), "P4\n32 32\n");

    memset(pbm + n, 0x55, 128);
    CHECK_EQ(seal_icon(pbm), RIMG_OK);
    pbm[n + 127] = '\0';
    CHECK_EQ(seal_icon(pbm), RIMG_E_SIZE);
    CHECK_EQ(seal_icon("P4\n32 32"), RIMG_E_IO);
}

TEST(icon_format_outside_enum)
{
    static uint8_t img[4096], out[4096];
    volatile int bad_format = 7;
    rimg_seal_opts_t s;
    size_t n = holo_image(img);

    memset(&s, 0, sizeof(s));
    s.icon = img;
    s.icon_len = 128;
    s.icon_format = (rimg_icon_format_t)bad_format;
    CHECK_EQ(rimg_seal(img, n, &s, out), RIMG_E_IO);
}

TEST(null_arguments)
{
    static uint8_t img[4096], out[4096];
    rimg_seal_opts_t s;
    rimg_pack_opts_t p;
    size_t n = holo_image(img), len = 99;

    memset(&s, 0, sizeof(s));
    memset(&p, 0, sizeof(p));
    p.magic = "BOOT";
    p.load_addr = 0x8000;
    p.subtype = 7;
    CHECK_EQ(rimg_seal(rc_launder(NULL), n, &s, out), RIMG_E_IO);
    CHECK_EQ(rimg_seal(img, n, rc_launder(NULL), out), RIMG_E_IO);
    CHECK_EQ(rimg_seal(img, n, &s, rc_launder(NULL)), RIMG_E_IO);
    CHECK_EQ(rimg_pack(rc_launder(NULL), 4, &p, out, sizeof(out), &len), RIMG_E_IO);
    CHECK_EQ(len, 0);
    len = 99;
    CHECK_EQ(rimg_pack(img, 4, rc_launder(NULL), out, sizeof(out), &len), RIMG_E_IO);
    CHECK_EQ(len, 0);
    CHECK_EQ(rimg_pack(img, 4, &p, out, sizeof(out), rc_launder(NULL)), RIMG_E_IO);
    p.meta = 2;
    len = 99;
    CHECK_EQ(rimg_pack(img, 4, &p, out, sizeof(out), &len), RIMG_E_IO);
    CHECK_EQ(len, 0);
}

TEST(pack_capacity)
{
    static uint8_t payload[64], out[4096];
    rimg_pack_opts_t p;
    size_t len = 99;

    memset(&p, 0, sizeof(p));
    p.magic = "BOOT";
    p.load_addr = 0x8000;
    p.subtype = 7;
    p.meta = -1;
    CHECK_EQ(rimg_pack(payload, sizeof(payload), &p, out, 0x60 + 63, &len), RIMG_E_IO);
    CHECK_EQ(len, 0);
    /* Earlier steps come first: a bad entry is size even with no room. */
    p.entry = 2;
    CHECK_EQ(rimg_pack(payload, sizeof(payload), &p, out, 0, &len), RIMG_E_SIZE);
    p.entry = 0;
    CHECK_EQ(rimg_pack(payload, sizeof(payload), &p, out, sizeof(out), &len), RIMG_OK);
    CHECK_EQ(len, 0x60 + sizeof(payload));
    CHECK_EQ(rimg_validate(out, len, NULL, NULL), RIMG_OK);
}

TEST(pack_payload_too_long)
{
    static uint8_t out[16];
    rimg_pack_opts_t p;
    size_t len = 99;

    if (sizeof(size_t) < 8) {
        return;
    }
    memset(&p, 0, sizeof(p));
    p.magic = "BOOT";
    p.load_addr = 0x8000;
    p.subtype = 7;
    p.meta = -1;
    /* The payload is never read: step 5 fails before the copy. */
    CHECK_EQ(rimg_pack(out, (size_t)0xFFFFFFFFu - 0x60 + 1, &p, out, sizeof(out), &len),
             RIMG_E_SIZE);
    CHECK_EQ(len, 0);
}

/* Seal step 9 comes before any META write: an icon on a short image is size. */
TEST(seal_meta_short_icon)
{
    static uint8_t img[4096], icon[128];
    rimg_seal_opts_t s;
    size_t len = 0xA4;
    uint8_t *out = malloc(len);

    holo_image(img);
    memset(img + 0x34, 0, 4); /* file_size 0: seal sets it */
    memset(&s, 0, sizeof(s));
    s.icon = icon;
    s.icon_len = sizeof(icon);
    CHECK(out != NULL);
    if (out != NULL) {
        CHECK_EQ(rimg_seal(img, len, &s, out), RIMG_E_SIZE);
        free(out);
    }
}
