/* BAD ON PURPOSE: test-host-gccwarn must reject this file.
 *
 * GCC at -O1 with -fsanitize=address,undefined, as CI builds the host
 * tests, loses the loop bound and reports -Wformat-truncation on the
 * snprintf. Without the sanitizers, at -O2, GCC sees i < 32 and stays
 * quiet. mk/host.mk compiles this file in its self-test and expects the
 * ci pass to fail. Never fix it and never add it to a test build.
 */
#include <stdio.h>

void gccwarn_bad_names(void);

void gccwarn_bad_names(void)
{
    char name[8];
    int i;

    for (i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "U%d", i);
        puts(name);
    }
}
