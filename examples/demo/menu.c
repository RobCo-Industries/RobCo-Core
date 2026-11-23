/*
 * The menu variant of the demo, built with -DDEMO_MENU=1: the new
 * components together, the way a product uses them. MMU and caches on,
 * the SD card mounted when one is present, a start item read from
 * /DEMO.INI on the card, a menu on the screen console driven by keys
 * from the serial console, a click through the audio backend on each key
 * press, and the clock when it is set.
 *
 * Markers: ev=config, ev=clock, ev=audio_init, ev=ready, one ev=menu per
 * highlight move, ev=select on Enter, each followed by a console dump.
 */
#include <stdint.h>

#include <robco/audio.h>
#include <robco/blk.h>
#include <robco/boot.h>
#include <robco/console.h>
#include <robco/datetime.h>
#include <robco/fb.h>
#include <robco/fs.h>
#include <robco/i2c.h>
#include <robco/ini.h>
#include <robco/input.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/mmu.h>
#include <robco/part.h>
#include <robco/rtc.h>
#include <robco/sd.h>
#include <robco/tick.h>
#include <robco/uart.h>

#include "menu.h"

#if defined(DEMO_MENU) && DEMO_MENU

#define TICK_HZ     100
#define CLICK_HZ    2000
#define CLICK_MS    10
#define CONFIG_PATH "sd:/DEMO.INI"
#define TITLE_MAX   40
#define ITEM_ROW    4

static const char *const items[] = { "Diagnostics", "Event log", "Set clock", "Reboot" };
#define ITEM_COUNT  ((int)(sizeof(items) / sizeof(items[0])))

static const char *const markers[] = { "/DEMO.INI" };
static fs_boot_t vol;
static char ini_text[1024];
static char title[TITLE_MAX + 1] = "ROBCO INDUSTRIES TERMLINK";

static void fail(const char *what, int rc)
{
    k_printf("@RC stage=demo ev=error what=%s rc=%d\n", what, rc);
    uart_flush();
    robco_halt();
}

/*
 * Mounts the card and reads the start item. Without a card, or without
 * the file, the menu starts on item 0. ev=config says which case ran.
 */
static int read_config(void)
{
    char value[INI_MAX_VALUE + 1];
    const char *mount = "none";
    const char *ini = "none";
    int32_t start = 0;
    size_t len = 0;
    int card = sd_init();

    if (card == 0) {
        /* The boot volume rule: the first FAT volume that holds DEMO.INI, else the first one. */
        int rc = fs_mount_boot("sd", sd_blkdev(), markers, 1, &vol);

        if (rc == PART_BOOT_OK) {
            mount = "ok";
        } else if (rc == PART_BOOT_ERR_MOUNT || rc == PART_BOOT_ERR_GPT || vol.last_err != 0) {
            mount = "error";
        }
    }
    if (mount[0] == 'o' && fs_read_file(CONFIG_PATH, ini_text, sizeof(ini_text), &len) == FS_OK) {
        ini = ini_get_int(ini_text, len, "menu", "start", 0, ITEM_COUNT - 1, &start) == 0 ?
              "ok" : "error";
        if (ini_get(ini_text, len, "menu", "title", value, sizeof(value)) == 0 &&
            value[0] != '\0') {
            k_strncpy(title, value, sizeof(title) - 1);
        }
    }
    k_printf("@RC stage=demo ev=config card=%s mount=%s ini=%s start=%d\n",
             card == 0 ? "ok" : "none", mount, ini, (int)start);
    return (int)start;
}

/* Seeds the software clock from an RTC when one answers. */
static void show_clock(void)
{
    datetime_t now;
    char iso[DT_ISO_SIZE];
    int set;

    if (i2c_init(100000) == 0) {
        rtc_init();
    }
    set = clock_get(&now) == 0 && dt_format_iso(&now, iso, sizeof(iso)) > 0;
    console_set_cursor(0, ITEM_ROW + ITEM_COUNT + 1);
    console_puts(set ? "Clock: " : "Clock: not set");
    if (set) {
        console_puts(iso);
    }
    k_printf("@RC stage=demo ev=clock state=%s\n", set ? "set" : "unset");
}

static void draw(int sel)
{
    int i;

    for (i = 0; i < ITEM_COUNT; i++) {
        console_set_cursor(0, (unsigned)(ITEM_ROW + i));
        console_set_attr(i == sel ? (CONSOLE_ATTR_BRIGHT | CONSOLE_ATTR_INVERSE)
                                  : CONSOLE_ATTR_NORMAL);
        console_puts(i == sel ? "> " : "  ");
        console_puts(items[i]);
        console_set_attr(CONSOLE_ATTR_NORMAL);
    }
}

static void click(void)
{
    int rc = audio_tone(CLICK_HZ, CLICK_MS);

    if (rc != 0) {
        k_printf("@RC stage=demo ev=error what=audio_tone rc=%d\n", rc);
    }
}

void demo_menu(void)
{
    input_event_t ev;
    int sel;
    int rc;

    rc = mmu_init();
    if (rc != 0) {
        fail("mmu_init", rc);
    }
    mmu_enable();
    k_printf("@RC stage=demo ev=mmu on=%d\n", mmu_enabled());

    rc = fb_init(640, 480, 32);
    if (rc != 0) {
        fail("fb_init", rc);
    }
    rc = console_init(CONSOLE_SCHEME_GREEN);
    if (rc != 0) {
        fail("console_init", rc);
    }

    irq_init();
    rc = tick_init(TICK_HZ);
    if (rc != 0) {
        fail("tick_init", rc);
    }
    irq_cpu_enable();

    sel = read_config();
    console_set_cursor(0, 0);
    console_puts(title);
    console_set_cursor(0, 2);
    console_puts("Select an item:");
    show_clock();

    rc = audio_init();
    k_printf("@RC stage=demo ev=audio_init rc=%d backend=%s\n", rc, audio_backend());
    if (rc != 0) {
        fail("audio_init", rc);
    }
    rc = input_init();
    if (rc == 0) {
        rc = input_serial_start(INPUT_SERIAL_MAIN);
    }
    if (rc != 0) {
        fail("input", rc);
    }

    draw(sel);
    k_printf("@RC stage=demo ev=ready sel=%d\n", sel);
    console_dump(uart_putc);
    for (;;) {
        audio_poll();
        if (!input_wait(&ev, 20)) {
            continue;
        }
        if (ev.type != INPUT_EV_KEY || ev.value != INPUT_PRESS) {
            continue;
        }
        click();
        if (ev.code == INPUT_KEY_UP || ev.code == INPUT_KEY_DOWN) {
            sel = (sel + (ev.code == INPUT_KEY_UP ? ITEM_COUNT - 1 : 1)) % ITEM_COUNT;
            draw(sel);
            k_printf("@RC stage=demo ev=menu sel=%d\n", sel);
        } else if (ev.code == INPUT_KEY_SELECT) {
            k_printf("@RC stage=demo ev=select sel=%d\n", sel);
        } else {
            continue;
        }
        console_dump(uart_putc);
    }
}

#endif
