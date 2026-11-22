#ifndef ROBCO_CONFIG_H
#define ROBCO_CONFIG_H

/* Preprocessor macros only: this file is also forced into asm and the linker script. */

#define ROBCO_STAGE_NAME        "input"

#define ROBCO_IMG_MAGIC         RIMG_MAGIC_BOOT
#define ROBCO_IMG_SUBTYPE       RIMG_BOOT_EXAMPLE
#define ROBCO_IMG_ABI_VERSION   (2 << 16)
#define ROBCO_IMG_VERSION       1
#define ROBCO_IMG_NAME          "input"

/* First image slot, 0x00008000 to 0x000FFFFF. The link fails if the image does not fit. */
#ifndef ROBCO_SLOT_END
#define ROBCO_SLOT_END          0x00100000
#endif

/*
 * Button and dial pins (BCM numbers). Buttons connect the pin to ground,
 * with the internal pull-up. The pins avoid:
 *   0, 1    ID EEPROM I2C
 *   2, 3    I2C1
 *   7 to 11 SPI0
 *   12, 13  PWM audio on some boards
 *   14, 15  UART (console and mini UART)
 *   18 to 21 PCM/I2S
 *   34 to 53 SD card, Wi-Fi SDIO and analog audio
 */
#define INPUT_PIN_UP            5
#define INPUT_PIN_DOWN          6
#define INPUT_PIN_LEFT          17
#define INPUT_PIN_RIGHT         22
#define INPUT_PIN_SELECT        26
#define INPUT_PIN_BACK          27
#define INPUT_PIN_DIAL_A        23
#define INPUT_PIN_DIAL_B        24
#define INPUT_PIN_DIAL_PUSH     16

#endif
