#ifndef ROBCO_CONFIG_H
#define ROBCO_CONFIG_H

/* Preprocessor macros only: this file is also forced into asm and the linker script. */

#define ROBCO_STAGE_NAME        "audio"

#define ROBCO_IMG_MAGIC         RIMG_MAGIC_BOOT
#define ROBCO_IMG_SUBTYPE       RIMG_BOOT_EXAMPLE
#define ROBCO_IMG_ABI_VERSION   (2 << 16)
#define ROBCO_IMG_VERSION       1
#define ROBCO_IMG_NAME          "audio"

/* First image slot, 0x00008000 to 0x000FFFFF. The link fails if the image does not fit. */
#ifndef ROBCO_SLOT_END
#define ROBCO_SLOT_END          0x00100000
#endif

/*
 * Audio output. Leave these undefined for the per-board default, or set
 * them for your wiring, for example GPIO 12 and 13 at ALT0 on a Pi Zero:
 *
 *   #define ROBCO_AUDIO_PIN0   12
 *   #define ROBCO_AUDIO_FN0    4
 *   #define ROBCO_AUDIO_PIN1   13
 *   #define ROBCO_AUDIO_FN1    4
 */

#endif
