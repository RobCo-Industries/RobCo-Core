#ifndef ROBCO_CRC32_H
#define ROBCO_CRC32_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC-32/ISO-HDLC, the zlib crc32(): reflected, polynomial 0xEDB88320,
 * init and xorout 0xFFFFFFFF. Stream by starting with crc = 0 and passing
 * each result back in.
 */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t crc32(const void *data, size_t len);

#endif
