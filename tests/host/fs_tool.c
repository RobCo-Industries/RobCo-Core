/*
 * Host CLI over the fs API, working on a disk image file.
 *
 *   fs_tool [-p N] IMAGE ls [DIR] | cat FILE | put HOSTFILE FILE | rm PATH |
 *                        mv FROM TO | mkdir DIR | df | mkfs [any|fat|fat32] | parts
 *
 * -p N uses entry N (from 0) of part_scan() instead of the whole image.
 * Exit status: 0 on success, 1 on an fs error (message on stderr), 2 on usage.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <robco/fs.h>
#include <robco/part.h>

#include "blk_file.h"

static int usage(void)
{
    fprintf(stderr, "usage: fs_tool [-p N] IMAGE ls [DIR] | cat FILE | put HOSTFILE FILE |\n"
                    "       rm PATH | mv FROM TO | mkdir DIR | df | mkfs [any|fat|fat32] | parts\n");
    return 2;
}

static int fail(const char *what, int rc)
{
    fprintf(stderr, "fs_tool: %s: %s\n", what, fs_strerror(rc));
    return 1;
}

static int cmd_ls(const char *path)
{
    fs_dir_t d;
    fs_stat_t st;
    int rc = fs_opendir(&d, path);

    if (rc != FS_OK) {
        return fail(path, rc);
    }
    while ((rc = fs_readdir(&d, &st)) == 1) {
        if (st.is_dir) {
            printf("d %s\n", st.name);
        } else {
            printf("f %lu %s\n", (unsigned long)st.size, st.name);
        }
    }
    fs_closedir(&d);
    return rc < 0 ? fail(path, rc) : 0;
}

static int cmd_cat(const char *path)
{
    fs_file_t f;
    char buf[4096];
    int rc = fs_open(&f, path, FS_READ);

    if (rc != FS_OK) {
        return fail(path, rc);
    }
    while ((rc = fs_read(&f, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)rc, stdout);
    }
    fs_close(&f);
    return rc < 0 ? fail(path, rc) : 0;
}

static int cmd_put(const char *host, const char *path)
{
    FILE *in = fopen(host, "rb");
    fs_file_t f;
    char buf[4096];
    size_t n;
    int rc;

    if (in == NULL) {
        perror(host);
        return 1;
    }
    rc = fs_open(&f, path, FS_WRITE | FS_CREATE | FS_TRUNC);
    while (rc == FS_OK && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        rc = fs_write(&f, buf, n);
        rc = rc == (int)n ? FS_OK : rc < 0 ? rc : FS_ERR_NOSPC;
    }
    fclose(in);
    if (rc != FS_OK) {
        fs_close(&f);
        return fail(path, rc);
    }
    rc = fs_close(&f);
    return rc != FS_OK ? fail(path, rc) : 0;
}

static int cmd_df(void)
{
    fs_statvfs_t st;
    int rc = fs_statvfs("img", &st);

    if (rc != FS_OK) {
        return fail("df", rc);
    }
    printf("FAT%u total %llu free %llu cluster %lu\n", st.fat_type,
           (unsigned long long)st.total_bytes, (unsigned long long)st.free_bytes,
           (unsigned long)st.cluster_bytes);
    return 0;
}

static int cmd_mkfs(blk_dev_t *dev, const char *kind)
{
    fs_mkfs_opts_t o = { FS_MKFS_ANY, 0 };
    int rc;

    if (kind != NULL && strcmp(kind, "fat") == 0) {
        o.fmt = FS_MKFS_FAT;
    } else if (kind != NULL && strcmp(kind, "fat32") == 0) {
        o.fmt = FS_MKFS_FAT32;
    } else if (kind != NULL && strcmp(kind, "any") != 0) {
        return usage();
    }
    rc = fs_mkfs(dev, &o);
    return rc != FS_OK ? fail("mkfs", rc) : 0;
}

static int cmd_parts(blk_dev_t *dev)
{
    part_info_t p[PART_MAX];
    int i, n = part_scan(dev, p, PART_MAX);

    if (n < 0) {
        fprintf(stderr, "fs_tool: parts: %s\n", blk_strerror(n));
        return 1;
    }
    for (i = 0; i < n; i++) {
        printf("%d slot %u type 0x%02x start %llu count %llu%s%s\n", i, p[i].index, p[i].type,
               (unsigned long long)p[i].start_lba, (unsigned long long)p[i].block_count,
               p[i].bootable ? " boot" : "", p[i].superfloppy ? " superfloppy" : "");
    }
    return 0;
}

static int run(blk_dev_t *dev, int argc, char **argv)
{
    const char *cmd = argv[0];
    int rc;

    if (strcmp(cmd, "mkfs") == 0 && argc <= 2) {
        return cmd_mkfs(dev, argc == 2 ? argv[1] : NULL);
    }
    if (strcmp(cmd, "parts") == 0 && argc == 1) {
        return cmd_parts(dev);
    }
    rc = fs_mount("img", dev);
    if (rc != FS_OK) {
        return fail("mount", rc);
    }
    if (strcmp(cmd, "ls") == 0 && argc <= 2) {
        rc = cmd_ls(argc == 2 ? argv[1] : "/");
    } else if (strcmp(cmd, "cat") == 0 && argc == 2) {
        rc = cmd_cat(argv[1]);
    } else if (strcmp(cmd, "put") == 0 && argc == 3) {
        rc = cmd_put(argv[1], argv[2]);
    } else if (strcmp(cmd, "rm") == 0 && argc == 2) {
        rc = fs_unlink(argv[1]);
        rc = rc != FS_OK ? fail(argv[1], rc) : 0;
    } else if (strcmp(cmd, "mv") == 0 && argc == 3) {
        rc = fs_rename(argv[1], argv[2]);
        rc = rc != FS_OK ? fail(argv[1], rc) : 0;
    } else if (strcmp(cmd, "mkdir") == 0 && argc == 2) {
        rc = fs_mkdir(argv[1]);
        rc = rc != FS_OK ? fail(argv[1], rc) : 0;
    } else if (strcmp(cmd, "df") == 0 && argc == 1) {
        rc = cmd_df();
    } else {
        rc = usage();
    }
    fs_unmount("img");
    return rc;
}

int main(int argc, char **argv)
{
    blk_dev_t file, sub, *dev = &file;
    part_info_t p[PART_MAX];
    long part = -1;
    int i = 1, rc;

    if (argc > 2 && strcmp(argv[1], "-p") == 0) {
        char *end;

        part = strtol(argv[2], &end, 10);
        if (*end != '\0' || part < 0 || part >= (long)PART_MAX) {
            return usage();
        }
        i = 3;
    }
    if (argc - i < 2) {
        return usage();
    }
    if (blk_file_open(&file, argv[i], 0) != 0) {
        perror(argv[i]);
        return 1;
    }
    if (part >= 0) {
        int n = part_scan(&file, p, PART_MAX);

        if (n <= part || part_open(&file, &p[part], &sub) != 0) {
            fprintf(stderr, "fs_tool: no partition %ld\n", part);
            blk_file_close(&file);
            return 1;
        }
        dev = &sub;
    }
    rc = run(dev, argc - i - 1, argv + i + 1);
    blk_file_close(&file);
    return rc;
}
