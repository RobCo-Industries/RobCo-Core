#ifndef ROBCO_INI_H
#define ROBCO_INI_H

#include <stddef.h>
#include <stdint.h>

/*
 * Strict INI parser for text held in memory. Pure, src/lib/ini.c.
 * No allocation and no global state: every call is reentrant.
 *
 * Syntax, one item per line, LF or CR LF line ends:
 *   [section]          name of [A-Za-z0-9_.-], spaces around it allowed
 *   key = value        key of [A-Za-z0-9_.-]
 *   # comment          also ';', whole line or trailing
 * Keys before any section belong to the section "".
 * An unquoted value runs to the end of the line, or to a '#' or ';' that
 * starts the value or follows a space or tab. "a#b" keeps its '#'.
 * A value in double quotes may hold any text, with \" \\ and \n escapes.
 * Spaces and tabs around names and values are dropped.
 */

#define INI_MAX_LINE        255     /* bytes per line, without the line end */
#define INI_MAX_SECTION     31      /* bytes per section name */
#define INI_MAX_KEY         31      /* bytes per key */
#define INI_MAX_VALUE       127     /* bytes per value, after escapes */

#define INI_STOPPED         1       /* the handler returned non-zero */

#define INI_ERR_ARG         (-1)    /* NULL text with a length, or no handler */
#define INI_ERR_SYNTAX      (-2)    /* line that is not a section, key or comment */
#define INI_ERR_TOO_LONG    (-3)    /* line, name, key or value above its limit */
#define INI_ERR_CHAR        (-4)    /* NUL, lone CR or other control byte */
#define INI_ERR_NOT_FOUND   (-5)    /* ini_get*: no such key */
#define INI_ERR_SPACE       (-6)    /* ini_get: value does not fit `out` */
#define INI_ERR_VALUE       (-7)    /* ini_get_int/bool: not a number or a boolean */
#define INI_ERR_RANGE       (-8)    /* ini_get_int: outside min..max */

/*
 * Called once per key in file order. The strings are NUL-terminated
 * copies that live until the handler returns. `line` counts from 1.
 * Return 0 to go on, non-zero to stop.
 */
typedef int (*ini_handler_t)(const char *section, const char *key,
                             const char *value, unsigned line, void *ctx);

/*
 * Parses `len` bytes of `text`. Returns 0 at the end, INI_STOPPED when the
 * handler stopped it, or an INI_ERR_* code. `err_line`, if not NULL,
 * receives the line of the error, or 0 when there is none.
 * Keys reach the handler up to the bad line: check the return code.
 */
int ini_parse(const char *text, size_t len, ini_handler_t fn, void *ctx, unsigned *err_line);

/*
 * Copies the value of `key` in `section` ("" for keys before any section)
 * into `out`. The last one wins when a key repeats. The whole text must
 * parse. Returns 0, INI_ERR_NOT_FOUND, INI_ERR_SPACE or a parse error.
 */
int ini_get(const char *text, size_t len, const char *section, const char *key,
            char *out, size_t out_size);

/* Decimal with an optional sign, or 0x hex. Checks min <= value <= max. */
int ini_get_int(const char *text, size_t len, const char *section, const char *key,
                int32_t min, int32_t max, int32_t *out);

/* true/false, yes/no, on/off, 1/0, any case. Stores 1 or 0. */
int ini_get_bool(const char *text, size_t len, const char *section, const char *key,
                 int *out);

/* Short text for an INI_* code. */
const char *ini_strerror(int code);

#endif
