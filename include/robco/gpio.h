#ifndef ROBCO_GPIO_H
#define ROBCO_GPIO_H

/*
 * BCM283x GPIO, pins 0 to 53. Every function returns a negative error
 * for a bad pin or argument. BCM2711 pulls are not supported.
 */

#define GPIO_PIN_COUNT  54

#define GPIO_ERR_PIN    (-1)    /* pin out of range */
#define GPIO_ERR_ARG    (-2)    /* bad function, pull or level */
#define GPIO_ERR_BOARD  (-3)    /* no GPIO block or unsupported SoC */

/* Function select codes, as written to GPFSEL. */
enum {
    GPIO_FN_INPUT  = 0,
    GPIO_FN_OUTPUT = 1,
    GPIO_FN_ALT0   = 4,
    GPIO_FN_ALT1   = 5,
    GPIO_FN_ALT2   = 6,
    GPIO_FN_ALT3   = 7,
    GPIO_FN_ALT4   = 3,
    GPIO_FN_ALT5   = 2,
};

/* Pull codes, as written to GPPUD. */
enum {
    GPIO_PULL_NONE = 0,
    GPIO_PULL_DOWN = 1,
    GPIO_PULL_UP   = 2,
};

/* Sets the pin's function. Only the pin's 3 GPFSEL bits change. */
int gpio_set_function(unsigned pin, unsigned fn);

/* Returns the pin's GPIO_FN_* code. */
int gpio_get_function(unsigned pin);

/*
 * Sets the pull. Returns GPIO_ERR_BOARD on BCM2711. Waits by counting loop
 * iterations, not on the system timer, so it works when the timer is stopped.
 */
int gpio_set_pull(unsigned pin, unsigned pull);

/* Drives the output latch: 0 low, anything else high. */
int gpio_write(unsigned pin, int level);

/* Returns the pin level from GPLEV, 0 or 1. */
int gpio_read(unsigned pin);

/* Inverts the current level. */
int gpio_toggle(unsigned pin);

/* Sets the level first, then switches to output, so the pin never glitches. */
int gpio_config_output(unsigned pin, int initial_level);

/* Switches to input and sets the pull. */
int gpio_config_input(unsigned pin, unsigned pull);

#endif
