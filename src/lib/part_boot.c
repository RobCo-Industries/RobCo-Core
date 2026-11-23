#include <robco/part.h>

int part_boot_class(const part_info_t *p)
{
    if (p->superfloppy) {
        return PART_BOOT_FAT;
    }
    switch (p->type) {
    case PART_TYPE_FAT12:
    case PART_TYPE_FAT16_SM:
    case PART_TYPE_FAT16:
    case PART_TYPE_FAT32:
    case PART_TYPE_FAT32_LBA:
    case PART_TYPE_FAT16_LBA:
        return PART_BOOT_FAT;
    case 0:
    case PART_TYPE_EXTENDED:
    case PART_TYPE_EXT_LBA:
    case PART_TYPE_GPT:
        return PART_BOOT_NEVER;
    default:
        return PART_BOOT_OTHER;
    }
}

unsigned part_boot_order(const part_info_t *parts, unsigned n, uint8_t order[PART_MAX])
{
    static const int pass[2] = { PART_BOOT_FAT, PART_BOOT_OTHER };
    unsigned k = 0;
    unsigned i, j;

    if (n > PART_MAX) {
        n = PART_MAX;
    }
    for (j = 0; j < 2; j++) {
        for (i = 0; i < n; i++) {
            if (part_boot_class(&parts[i]) == pass[j]) {
                order[k++] = (uint8_t)i;
            }
        }
    }
    return k;
}

int part_boot_pick(const uint8_t *probe, unsigned n)
{
    int mounted = -1;
    unsigned i;

    for (i = 0; i < n; i++) {
        if (probe[i] == PART_PROBE_MARKER) {
            return (int)i;
        }
        if (probe[i] == PART_PROBE_MOUNTED && mounted < 0) {
            mounted = (int)i;
        }
    }
    return mounted;
}

int part_boot_failure(int scan, const part_info_t *parts)
{
    int gpt = 0;
    int i;

    if (scan < 0) {
        return PART_BOOT_ERR_DEVICE;
    }
    for (i = 0; i < scan && i < (int)PART_MAX; i++) {
        if (part_boot_class(&parts[i]) == PART_BOOT_FAT) {
            return PART_BOOT_ERR_MOUNT;
        }
        if (!parts[i].superfloppy && parts[i].type == PART_TYPE_GPT) {
            gpt = 1;
        }
    }
    return gpt ? PART_BOOT_ERR_GPT : PART_BOOT_ERR_NOFAT;
}

const char *part_boot_strerror(int rc)
{
    switch (rc) {
    case PART_BOOT_OK:         return "ok";
    case PART_BOOT_ERR_DEVICE: return "nodev";
    case PART_BOOT_ERR_NOFAT:  return "nofat";
    case PART_BOOT_ERR_GPT:    return "gpt";
    case PART_BOOT_ERR_MOUNT:  return "mount";
    case PART_BOOT_ERR_ARG:    return "arg";
    case PART_BOOT_ERR_BUSY:   return "busy";
    default:                   return "unknown";
    }
}
