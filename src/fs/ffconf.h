/*
 * FatFs configuration for RobCo-Core. ff.h includes "ffconf.h"; the build
 * puts src/fs/ on the include path and third_party/fatfs/ has no ffconf.h,
 * so this file is the one used. See third_party/fatfs/README.md.
 *
 * ROBCO_FS_READONLY set to 1 in robco_config.h gives the read-only flavour:
 * no write, mkfs, rename or mkdir code.
 */
#define FFCONF_DEF 80386 /* must match FF_DEFINED in ff.h */

#if defined(ROBCO_FS_READONLY) && ROBCO_FS_READONLY
#define FF_FS_READONLY 1
#define FF_USE_MKFS    0
#else
#define FF_FS_READONLY 0
#define FF_USE_MKFS    1
#endif

#define FF_FS_MINIMIZE  0
#define FF_USE_FIND     0
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    0
#define FF_USE_LABEL    0
#define FF_USE_FORWARD  0
#define FF_USE_STRFUNC  0
#define FF_PRINT_LLI    0
#define FF_PRINT_FLOAT  0
#define FF_STRF_ENCODE  0

#define FF_CODE_PAGE 437

/* Long names with one static working buffer in BSS: no heap, not reentrant. */
#define FF_USE_LFN     1
#define FF_MAX_LFN     255
#define FF_LFN_UNICODE 2 /* names are UTF-8 on the API */
#define FF_LFN_BUF     255
#define FF_SFN_BUF     12

/* "." and ".." in paths and a current directory per volume. No f_getcwd. */
#define FF_FS_RPATH   1
#define FF_PATH_DEPTH 10

/* The glue maps our volume names to drive numbers "0:", "1:" and so on. */
#if defined(ROBCO_FS_VOLUMES)
#define FF_VOLUMES         ROBCO_FS_VOLUMES
#else
#define FF_VOLUMES         2
#endif
#define FF_STR_VOLUME_ID   0
#define FF_MULTI_PARTITION 0

#define FF_MIN_SS   512
#define FF_MAX_SS   512
#define FF_LBA64    0
#define FF_MIN_GPT  0x10000000
#define FF_USE_TRIM 0

#define FF_FS_TINY  0
#define FF_FS_EXFAT 0

/* Time stamps come from fs_time_hook() in fs_glue.c: the clock when set, else 2026-01-01. */
#define FF_FS_NORTC   0
#define FF_NORTC_MON  1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2026

#define FF_FS_CRTIME   0
#define FF_FS_NOFSINFO 0

#define FF_FS_LOCK      0
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT   1000
