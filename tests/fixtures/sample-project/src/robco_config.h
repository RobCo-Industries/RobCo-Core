#ifndef ROBCO_CONFIG_H
#define ROBCO_CONFIG_H

/* Preprocessor macros only: this file is also forced into asm and the linker script. */

#define ROBCO_STAGE_NAME        "sample"
#define ROBCO_FIRST_STAGE       1
#define ROBCO_IMG_NAME          "sample"

#ifndef ROBCO_SLOT_END
#define ROBCO_SLOT_END          0x00100000
#endif

#endif
