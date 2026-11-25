#include <robco/blk.h>
#include <robco/sd.h>

/* Shared by the EMMC driver and the holotape slot. Pure, so host tests link it. */
const char *sd_strerror(int err)
{
    switch (err) {
    case 0:              return "ok";
    case SD_ERR_BOARD:   return "board";
    case SD_ERR_CLOCK:   return "clock";
    case SD_ERR_RESET:   return "reset";
    case SD_ERR_NOCARD:  return "nocard";
    case SD_ERR_VOLTAGE: return "voltage";
    case SD_ERR_BUSY:    return "busy";
    case SD_ERR_CMD:     return "cmd";
    case SD_ERR_CSD:     return "csd";
    case SD_ERR_OLD:     return "old";
    case SD_ERR_AGAIN:   return "again";
    case SD_ERR_BUS:     return "bus";
    default:             return blk_strerror(err);
    }
}
