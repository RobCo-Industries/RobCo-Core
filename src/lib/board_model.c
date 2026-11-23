#include <robco/board.h>

#define NEW_STYLE (1u << 23)

/*
 * New-style board types, bits 11:4 of the code. From the public Raspberry
 * Pi revision code table. Types RobCo-Core does not support (CM, 4, 400,
 * 5) still get their name.
 */
static const char *new_style(uint32_t type)
{
    switch (type) {
    case 0x00: return "Raspberry Pi 1 Model A";
    case 0x01: return "Raspberry Pi 1 Model B";
    case 0x02: return "Raspberry Pi 1 Model A+";
    case 0x03: return "Raspberry Pi 1 Model B+";
    case 0x04: return "Raspberry Pi 2 Model B";
    case 0x06: return "Raspberry Pi Compute Module 1";
    case 0x08: return "Raspberry Pi 3 Model B";
    case 0x09: return "Raspberry Pi Zero";
    case 0x0a: return "Raspberry Pi Compute Module 3";
    case 0x0c: return "Raspberry Pi Zero W";
    case 0x0d: return "Raspberry Pi 3 Model B+";
    case 0x0e: return "Raspberry Pi 3 Model A+";
    case 0x10: return "Raspberry Pi Compute Module 3+";
    case 0x11: return "Raspberry Pi 4 Model B";
    case 0x12: return "Raspberry Pi Zero 2 W";
    case 0x13: return "Raspberry Pi 400";
    case 0x14: return "Raspberry Pi Compute Module 4";
    case 0x17: return "Raspberry Pi 5";
    default:   return BOARD_MODEL_UNKNOWN;
    }
}

/* Old-style codes, the low 24 bits. Bit 24 is the warranty bit and is ignored. */
static const char *old_style(uint32_t code)
{
    switch (code) {
    case 0x0002: case 0x0003: case 0x0004: case 0x0005: case 0x0006:
    case 0x000d: case 0x000e: case 0x000f:
        return "Raspberry Pi 1 Model B";
    case 0x0007: case 0x0008: case 0x0009:
        return "Raspberry Pi 1 Model A";
    case 0x0010: case 0x0013:
        return "Raspberry Pi 1 Model B+";
    case 0x0012: case 0x0015:
        return "Raspberry Pi 1 Model A+";
    case 0x0011: case 0x0014:
        return "Raspberry Pi Compute Module 1";
    default:
        return BOARD_MODEL_UNKNOWN;
    }
}

const char *board_model_name(uint32_t revision)
{
    if (revision & NEW_STYLE) {
        return new_style((revision >> 4) & 0xffu);
    }
    return old_style(revision & 0x00ffffffu);
}
