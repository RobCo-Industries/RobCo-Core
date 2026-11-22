#include <stdint.h>

#include <robco/blk.h>
#include <robco/crc32.h>
#include <robco/datetime.h>
#include <robco/fs.h>
#include <robco/i2c.h>
#include <robco/ini.h>
#include <robco/klibc.h>
#include <robco/part.h>
#include <robco/rtc.h>
#include <robco/sd.h>
#include <robco/uart.h>

/*
 * SD card, partition table and FAT file system on the target. After the
 * mount it reads one command per line and answers each with one marker:
 *
 *   ls <dir>                    one ev=entry per entry, then ev=ls;
 *                               left=1 marks an atomic-write work file
 *   cat <path>                  size and CRC-32 of the file
 *   put <path> <size> <seed>    write the pattern below
 *   atomic <path> <size> <seed> the same through fs_write_file_atomic()
 *   recover <path>              fs_atomic_recover() after a power cut
 *   rm <path>, mv <a> <b>, mkdir <path>
 *   df, sync, umount, mount
 *   ini <path> <section> <key>  read the file and look the key up
 *
 * rc= is the error name from fs_rc_name() below, or an INI name for ini.
 */

#define STAGE    "storage"
#define VOL      "sd"
#define LINE_MAX 640
#define ARGS_MAX 4
#define BUF_SIZE (128u * 1024u)
#define CHUNK    (16u * 1024u)

static uint8_t buf[BUF_SIZE] __attribute__((aligned(8)));
static part_info_t part;
static blk_dev_t part_dev;
static int have_part;

/*
 * Pattern for put and atomic: byte i of the file is
 *   ((seed + i) * 2654435761 mod 2^32) >> 24
 */
static void fill_pattern(uint8_t *p, uint32_t off, uint32_t len, uint32_t seed)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        p[i] = (uint8_t)(((seed + off + i) * 2654435761u) >> 24);
    }
}

static const char *fs_rc_name(int rc)
{
    if (rc >= 0) {
        return "ok";
    }
    switch (rc) {
    case FS_ERR_IO:          return "io";
    case FS_ERR_TIMEOUT:     return "timeout";
    case FS_ERR_NOTREADY:    return "notready";
    case FS_ERR_RO:          return "ro";
    case FS_ERR_INVAL:       return "inval";
    case FS_ERR_NOENT:       return "noent";
    case FS_ERR_EXIST:       return "exist";
    case FS_ERR_DENIED:      return "denied";
    case FS_ERR_ISDIR:       return "isdir";
    case FS_ERR_NAME:        return "name";
    case FS_ERR_NAMETOOLONG: return "nametoolong";
    case FS_ERR_NOSPC:       return "nospc";
    case FS_ERR_NOVOL:       return "novol";
    case FS_ERR_NOFS:        return "nofs";
    case FS_ERR_BUSY:        return "busy";
    case FS_ERR_BADF:        return "badf";
    case FS_ERR_CORRUPT:     return "corrupt";
    case FS_ERR_NOMEM:       return "nomem";
    case FS_ERR_MKFS:        return "mkfs";
    case FS_ERR_TOOBIG:      return "toobig";
    case FS_ERR_MOUNTED:     return "mounted";
    default:                 return "unknown";
    }
}

static const char *ini_rc_name(int rc)
{
    switch (rc) {
    case 0:                 return "ok";
    case INI_ERR_ARG:       return "ini_arg";
    case INI_ERR_SYNTAX:    return "ini_syntax";
    case INI_ERR_TOO_LONG:  return "ini_toolong";
    case INI_ERR_CHAR:      return "ini_char";
    case INI_ERR_NOT_FOUND: return "ini_notfound";
    case INI_ERR_SPACE:     return "ini_space";
    default:                return "ini_other";
    }
}

static int read_line(char *line)
{
    int n = 0;

    for (;;) {
        int c = uart_getc();

        if (c == '\r' || c == '\n') {
            if (n > 0) {
                line[n] = 0;
                return n;
            }
            continue;
        }
        if (n < LINE_MAX - 1) {
            line[n++] = (char)c;
        }
    }
}

/* Splits line in place on spaces. Returns the number of words. */
static int split(char *line, char **argv, int max)
{
    int n = 0;

    while (*line != '\0' && n < max) {
        while (*line == ' ') {
            *line++ = '\0';
        }
        if (*line == '\0') {
            break;
        }
        argv[n++] = line;
        while (*line != '\0' && *line != ' ') {
            line++;
        }
    }
    return n;
}

static uint32_t num(const char *s)
{
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s++ - '0');
    }
    return v;
}

static int mount(void)
{
    if (!have_part) {
        /* A card without a partition table or FAT boot sector has no file system. */
        return sd_blkdev() != NULL ? FS_ERR_NOFS : FS_ERR_NOTREADY;
    }
    return fs_mount(VOL, &part_dev);
}

/* k_printf() has no 64-bit conversion. Writes v in decimal to out, 21 bytes. */
static const char *u64_dec(uint64_t v, char *out)
{
    char *p = out + 20;

    *p = '\0';
    do {
        *--p = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    } while (v != 0);
    return p;
}

/* Sizes in bytes. With ROBCO_FS_READONLY, df gives rc=ro and zeros. */
static void print_df(const char *ev, int rc)
{
    fs_statvfs_t st = { 0 };
    char total[21];
    char avail[21];

    if (rc == FS_OK) {
        int src = fs_statvfs(VOL, &st);

        /* The read-only build has no free count. The mount itself worked. */
        if (src != FS_ERR_RO || k_strcmp(ev, "mount") != 0) {
            rc = src;
        }
    }
    k_printf("@RC stage=" STAGE " ev=%s rc=%s total=%s free=%s cluster=%u fat=%u\n", ev,
             fs_rc_name(rc), u64_dec(st.total_bytes, total), u64_dec(st.free_bytes, avail),
             (unsigned)st.cluster_bytes, (unsigned)st.fat_type);
}

static void cmd_ls(const char *dir)
{
    fs_dir_t d;
    fs_stat_t st;
    unsigned count = 0;
    int rc = fs_opendir(&d, dir);

    while (rc == FS_OK && (rc = fs_readdir(&d, &st)) == 1) {
        k_printf("@RC stage=" STAGE
                 " ev=entry name=%s size=%u dir=%u date=0x%04x time=0x%04x left=%u\n",
                 st.name, (unsigned)st.size, (unsigned)st.is_dir, (unsigned)st.date,
                 (unsigned)st.time, (unsigned)st.leftover);
        count++;
        rc = FS_OK;
    }
    fs_closedir(&d);
    k_printf("@RC stage=" STAGE " ev=ls rc=%s count=%u\n", fs_rc_name(rc), count);
}

static void cmd_cat(const char *path)
{
    fs_file_t f;
    uint32_t crc = 0;
    uint32_t size = 0;
    int rc = fs_open(&f, path, FS_READ);

    while (rc == FS_OK) {
        rc = fs_read(&f, buf, CHUNK);
        if (rc <= 0) {
            break;
        }
        crc = crc32_update(crc, buf, (size_t)rc);
        size += (uint32_t)rc;
        rc = FS_OK;
    }
    fs_close(&f);
    k_printf("@RC stage=" STAGE " ev=cat rc=%s size=%u crc=0x%08x\n", fs_rc_name(rc),
             (unsigned)size, (unsigned)crc);
}

static void cmd_put(const char *path, uint32_t size, uint32_t seed)
{
    fs_file_t f;
    uint32_t done = 0;
    int rc = fs_open(&f, path, FS_WRITE | FS_CREATE | FS_TRUNC);

    while (rc == FS_OK && done < size) {
        uint32_t len = size - done < CHUNK ? size - done : CHUNK;

        fill_pattern(buf, done, len, seed);
        rc = fs_write(&f, buf, len);
        if (rc < 0) {
            break;
        }
        done += (uint32_t)rc;
        rc = (uint32_t)rc == len ? FS_OK : FS_ERR_NOSPC;
    }
    if (rc == FS_OK) {
        rc = fs_close(&f);
    } else {
        fs_close(&f);
    }
    k_printf("@RC stage=" STAGE " ev=put rc=%s size=%u\n", fs_rc_name(rc), (unsigned)done);
}

static void cmd_atomic(const char *path, uint32_t size, uint32_t seed)
{
    int rc = FS_ERR_TOOBIG;

    if (size <= BUF_SIZE) {
        fill_pattern(buf, 0, size, seed);
        rc = fs_write_file_atomic(path, buf, size);
    }
    k_printf("@RC stage=" STAGE " ev=atomic rc=%s size=%u\n", fs_rc_name(rc), (unsigned)size);
}

static void cmd_ini(const char *path, const char *section, const char *key)
{
    char value[INI_MAX_VALUE + 1];
    size_t len = 0;
    const char *name;
    int rc = fs_read_file(path, buf, BUF_SIZE, &len);

    value[0] = '\0';
    if (rc != FS_OK) {
        name = fs_rc_name(rc);
    } else {
        name = ini_rc_name(ini_get((const char *)buf, len, section, key, value, sizeof(value)));
    }
    k_printf("@RC stage=" STAGE " ev=ini rc=%s value=%s\n", name, value[0] ? value : "-");
}

static void command(char *line)
{
    char *argv[ARGS_MAX];
    int argc = split(line, argv, ARGS_MAX);
    int rc;

    if (argc == 2 && k_strcmp(argv[0], "ls") == 0) {
        cmd_ls(argv[1]);
    } else if (argc == 2 && k_strcmp(argv[0], "cat") == 0) {
        cmd_cat(argv[1]);
    } else if (argc == 4 && k_strcmp(argv[0], "put") == 0) {
        cmd_put(argv[1], num(argv[2]), num(argv[3]));
    } else if (argc == 4 && k_strcmp(argv[0], "atomic") == 0) {
        cmd_atomic(argv[1], num(argv[2]), num(argv[3]));
    } else if (argc == 2 && k_strcmp(argv[0], "recover") == 0) {
        k_printf("@RC stage=" STAGE " ev=recover rc=%s\n", fs_rc_name(fs_atomic_recover(argv[1])));
    } else if (argc == 2 && k_strcmp(argv[0], "rm") == 0) {
        k_printf("@RC stage=" STAGE " ev=rm rc=%s\n", fs_rc_name(fs_unlink(argv[1])));
    } else if (argc == 3 && k_strcmp(argv[0], "mv") == 0) {
        k_printf("@RC stage=" STAGE " ev=mv rc=%s\n", fs_rc_name(fs_rename(argv[1], argv[2])));
    } else if (argc == 2 && k_strcmp(argv[0], "mkdir") == 0) {
        k_printf("@RC stage=" STAGE " ev=mkdir rc=%s\n", fs_rc_name(fs_mkdir(argv[1])));
    } else if (argc == 1 && k_strcmp(argv[0], "df") == 0) {
        print_df("df", FS_OK);
    } else if (argc == 4 && k_strcmp(argv[0], "ini") == 0) {
        cmd_ini(argv[1], argv[2], argv[3]);
    } else if (argc == 1 && k_strcmp(argv[0], "sync") == 0) {
        rc = sd_blkdev() != NULL ? blk_sync(sd_blkdev()) : BLK_ERR_NOMEDIA;
        k_printf("@RC stage=" STAGE " ev=sync rc=%s\n", blk_strerror(rc));
    } else if (argc == 1 && k_strcmp(argv[0], "umount") == 0) {
        k_printf("@RC stage=" STAGE " ev=umount rc=%s\n", fs_rc_name(fs_unmount(VOL)));
    } else if (argc == 1 && k_strcmp(argv[0], "mount") == 0) {
        print_df("mount", mount());
    } else {
        k_printf("@RC stage=" STAGE " ev=error what=unknown_command\n");
    }
}

/* Seeds the software clock from the DS1307 when one answers. */
static void start_clock(void)
{
    datetime_t now;
    char iso[DT_ISO_SIZE];
    int rc = i2c_init(100000);

    if (rc == 0) {
        rc = rtc_init();
    }
    if (clock_get(&now) != 0 || dt_format_iso(&now, iso, sizeof(iso)) < 0) {
        k_strncpy(iso, "unset", sizeof(iso));
    }
    k_printf("@RC stage=" STAGE " ev=clock rc=%d now=%s\n", rc, iso);
}

void robco_main(void)
{
    static char line[LINE_MAX];
    const sd_info_t *in;
    int rc;
    int n;

    uart_init(115200);
    k_printf("\n@RC stage=" STAGE " ev=boot\n");
    start_clock();

    rc = sd_init();
    in = sd_info();
    k_printf("@RC stage=" STAGE " ev=card rc=%s type=%s blocks=%u\n", sd_strerror(rc),
             in == NULL ? "none" : in->type == SD_TYPE_SDHC ? "sdhc" : "sdsc",
             in == NULL ? 0u : (unsigned)in->capacity_blocks);

    n = rc == 0 ? part_scan(sd_blkdev(), &part, 1) : BLK_ERR_NOMEDIA;
    if (n < 0) {
        k_printf("@RC stage=" STAGE " ev=part rc=%s n=0\n", blk_strerror(n));
    } else {
        k_printf("@RC stage=" STAGE " ev=part rc=ok n=%d p0_type=0x%02x p0_lba=%u p0_blocks=%u\n",
                 n, n > 0 ? (unsigned)part.type : 0u, n > 0 ? (unsigned)part.start_lba : 0u,
                 n > 0 ? (unsigned)part.block_count : 0u);
    }
    if (n > 0 && part_open(sd_blkdev(), &part, &part_dev) == 0) {
        have_part = 1;
    }
    print_df("mount", mount());

    k_printf("@RC stage=" STAGE " ev=ready\n");
    for (;;) {
        read_line(line);
        command(line);
    }
}
