#ifndef SPIKES_CRC32_H
#define SPIKES_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32/ISO-HDLC (zlib, Ethernet). */
uint32_t crc32_iso_hdlc(const uint8_t *data, size_t len);

#endif
