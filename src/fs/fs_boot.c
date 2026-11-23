/*
 * The boot volume search. part_boot.c in src/lib holds the decisions;
 * this file only mounts entries and looks for the markers.
 */
#include <robco/fs.h>
#include <robco/klibc.h>
#include <robco/part.h>

#include "fs_priv.h"

/* Nonzero when volume is mounted, or a mounted volume uses out's device. */
static int in_use(const char *volume, const fs_boot_t *out)
{
    unsigned i;

    for (i = 0; i < FF_VOLUMES; i++) {
        if (fs_vols[i].dev == &out->dev ||
            (fs_vols[i].name[0] != '\0' && k_strcmp(fs_vols[i].name, volume) == 0)) {
            return 1;
        }
    }
    return 0;
}

/* Mounts parts[i] as volume, with the device and a copy of the entry in out. */
static int try_mount(const char *volume, blk_dev_t *dev, const part_info_t *p, fs_boot_t *out)
{
    int rc;

    out->part = *p;
    if (p->superfloppy) {
        return fs_mount(volume, dev);
    }
    rc = part_open(dev, &out->part, &out->dev);
    if (rc != 0) {
        return FS_ERR_IO;
    }
    return fs_mount(volume, &out->dev);
}

/* Nonzero when the mounted volume holds one marker as a non-empty regular file. */
static int has_marker(const char *volume, const char *const *markers, unsigned n)
{
    char path[FS_PATH_MAX + 1];
    size_t vlen = k_strlen(volume);
    fs_stat_t st;
    unsigned i;

    for (i = 0; i < n; i++) {
        size_t mlen = markers[i] != NULL ? k_strlen(markers[i]) : 0;

        if (mlen == 0 || vlen + 1 + mlen > FS_PATH_MAX) {
            continue;
        }
        memcpy(path, volume, vlen);
        path[vlen] = ':';
        memcpy(path + vlen + 1, markers[i], mlen + 1);
        if (fs_stat(path, &st) == FS_OK && !st.is_dir && (st.attr & FS_ATTR_DIR) == 0 &&
            st.size > 0) {
            return 1;
        }
    }
    return 0;
}

static void fill(fs_boot_t *out, uint8_t probe)
{
    const part_info_t *p = &out->part;

    out->slot = p->superfloppy ? FS_BOOT_WHOLE : p->index;
    out->type = p->superfloppy ? 0 : p->type;
    out->marker = probe == PART_PROBE_MARKER;
    out->by_type = part_boot_class(p) == PART_BOOT_FAT;
    out->start_lba = p->start_lba;
    out->block_count = p->block_count;
}

int fs_mount_boot(const char *volume, blk_dev_t *dev, const char *const *markers,
                  unsigned n_markers, fs_boot_t *out)
{
    part_info_t parts[PART_MAX];
    uint8_t order[PART_MAX];
    uint8_t probe[PART_MAX];
    unsigned n, i;
    int scan, pick, rc;

    if (out == NULL || volume == NULL || (markers == NULL && n_markers != 0)) {
        return PART_BOOT_ERR_ARG;
    }
    if (in_use(volume, out)) {
        return PART_BOOT_ERR_BUSY; /* out may hold a live device: leave it alone */
    }
    k_memset(out, 0, sizeof(*out));
    if (dev == NULL) {
        return PART_BOOT_ERR_DEVICE;
    }
    scan = part_scan(dev, parts, PART_MAX);
    if (scan < 0) {
        out->last_err = scan;
        return PART_BOOT_ERR_DEVICE;
    }
    n = part_boot_order(parts, (unsigned)scan, order);
    for (i = 0; i < n; i++) {
        rc = try_mount(volume, dev, &parts[order[i]], out);
        if (rc != FS_OK) {
            out->last_err = rc;
            probe[i] = PART_PROBE_NOMOUNT;
            continue;
        }
        probe[i] = has_marker(volume, markers, n_markers) ? PART_PROBE_MARKER
                                                          : PART_PROBE_MOUNTED;
        if (probe[i] == PART_PROBE_MARKER || n_markers == 0) {
            fill(out, probe[i]);
            return PART_BOOT_OK;
        }
        fs_unmount(volume);
    }
    /* No marker anywhere: mount the first volume that mounted again. */
    pick = part_boot_pick(probe, n);
    if (pick >= 0) {
        rc = try_mount(volume, dev, &parts[order[pick]], out);
        if (rc == FS_OK) {
            fill(out, probe[pick]);
            return PART_BOOT_OK;
        }
        out->last_err = rc;
    }
    return part_boot_failure(scan, parts);
}
