/* Clean unless GCCWARN_BREAK is defined, then GCC warns.
 *
 * tests/tools/test_sample_project.py compiles it in test-host-gccwarn,
 * then sets the define on the command line. The check must compile it
 * again and fail, although the file did not change.
 */
void gccwarn_gated(void);

void gccwarn_gated(void)
{
#ifdef GCCWARN_BREAK
    int unused;
#endif
}
