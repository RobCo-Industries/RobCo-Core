#include "greet.h"
#include "rc_test.h"

TEST(formats)
{
    char buf[32];

    CHECK_EQ(greet(buf, sizeof(buf), "pi", 7), 11);
    CHECK_STR(buf, "hello pi #7");
}

TEST(host_define)
{
    CHECK_EQ(SAMPLE_HOST, 1);
}
