#include <robco/board.h>

#include "rc_test.h"

/* Real revision codes, from the public Raspberry Pi revision code table. */
static const struct {
    uint32_t rev;
    const char *name;
} known[] = {
    { 0x900092u, "Raspberry Pi Zero" },        /* 512 MB, Sony, 1.2 */
    { 0x900093u, "Raspberry Pi Zero" },        /* 1.3 */
    { 0x920093u, "Raspberry Pi Zero" },        /* Embest 1.3 */
    { 0x920092u, "Raspberry Pi Zero" },        /* QEMU raspi0 */
    { 0x9000c1u, "Raspberry Pi Zero W" },
    { 0x902120u, "Raspberry Pi Zero 2 W" },
    { 0x900021u, "Raspberry Pi 1 Model A+" },  /* 512 MB, QEMU raspi1ap */
    { 0x0012u, "Raspberry Pi 1 Model A+" },
    { 0x0015u, "Raspberry Pi 1 Model A+" },
    { 0x0010u, "Raspberry Pi 1 Model B+" },
    { 0x0013u, "Raspberry Pi 1 Model B+" },
    { 0x900032u, "Raspberry Pi 1 Model B+" },
    { 0x1000010u, "Raspberry Pi 1 Model B+" }, /* warranty bit set */
    { 0x000eu, "Raspberry Pi 1 Model B" },
    { 0x0002u, "Raspberry Pi 1 Model B" },
    { 0x0008u, "Raspberry Pi 1 Model A" },
    { 0xa01040u, "Raspberry Pi 2 Model B" },
    { 0xa01041u, "Raspberry Pi 2 Model B" },
    { 0xa21041u, "Raspberry Pi 2 Model B" },   /* QEMU raspi2b */
    { 0xa22042u, "Raspberry Pi 2 Model B" },   /* BCM2837 1.2 */
    { 0xa02082u, "Raspberry Pi 3 Model B" },   /* QEMU raspi3b */
    { 0xa22082u, "Raspberry Pi 3 Model B" },
    { 0xa32082u, "Raspberry Pi 3 Model B" },
    { 0xa020d3u, "Raspberry Pi 3 Model B+" },
    { 0x9020e0u, "Raspberry Pi 3 Model A+" },
    { 0xa03111u, "Raspberry Pi 4 Model B" },
};

TEST(known_codes)
{
    size_t i;

    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        CHECK_STR(board_model_name(known[i].rev), known[i].name);
    }
}

TEST(unknown_codes)
{
    CHECK_STR(board_model_name(0), BOARD_MODEL_UNKNOWN);
    CHECK_STR(board_model_name(0x0001u), BOARD_MODEL_UNKNOWN);
    CHECK_STR(board_model_name(0x0016u), BOARD_MODEL_UNKNOWN);
    CHECK_STR(board_model_name(0x800ff0u), BOARD_MODEL_UNKNOWN);
    CHECK_STR(board_model_name(0xffffffffu), BOARD_MODEL_UNKNOWN);
    /* Stable: the same pointer every time. */
    CHECK(board_model_name(0) == board_model_name(0x800ff0u));
}
