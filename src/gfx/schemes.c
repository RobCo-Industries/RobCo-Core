#include <stddef.h>

#include <robco/console.h>

/*
 * Phosphor-style schemes. The background is a dark tint of the phosphor
 * colour, never pure black. RGB values:
 *
 *   scheme  background  dim      normal   bright
 *   green   #0a1a0e     #1f8a3d  #33e066  #b0ffc8
 *   amber   #1a1006     #8a5a10  #ffb000  #ffe0a0
 *   blue    #08101e     #2a5a9a  #5ab4ff  #c8e6ff
 *   white   #121214     #808084  #d8d8dc  #ffffff
 */
static const console_scheme_t schemes[CONSOLE_SCHEME_COUNT] = {
    [CONSOLE_SCHEME_GREEN] = {"green", {0x0a, 0x1a, 0x0e}, {0x1f, 0x8a, 0x3d},
                              {0x33, 0xe0, 0x66}, {0xb0, 0xff, 0xc8}},
    [CONSOLE_SCHEME_AMBER] = {"amber", {0x1a, 0x10, 0x06}, {0x8a, 0x5a, 0x10},
                              {0xff, 0xb0, 0x00}, {0xff, 0xe0, 0xa0}},
    [CONSOLE_SCHEME_BLUE] = {"blue", {0x08, 0x10, 0x1e}, {0x2a, 0x5a, 0x9a},
                             {0x5a, 0xb4, 0xff}, {0xc8, 0xe6, 0xff}},
    [CONSOLE_SCHEME_WHITE] = {"white", {0x12, 0x12, 0x14}, {0x80, 0x80, 0x84},
                              {0xd8, 0xd8, 0xdc}, {0xff, 0xff, 0xff}},
};

const console_scheme_t *console_scheme(unsigned scheme)
{
    return scheme < CONSOLE_SCHEME_COUNT ? &schemes[scheme] : NULL;
}
