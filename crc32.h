#if !defined(CRC32_H)
#define CRC32_H

#include <inttypes.h>

uint32_t calculate_crc32c(uint32_t crc32c, const unsigned char *buffer, unsigned int length);

#endif
