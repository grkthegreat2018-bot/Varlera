/* lzss.h - tiny LZSS compressor for the packed block-record stream.
 * Token format: flag byte per group of 8 items; bit=0 literal,
 * bit=1 match = u16 (off<<4 | len-3), window 4095, len 3..18. */
#ifndef LZSS_H
#define LZSS_H

#include <stddef.h>
#include <stdint.h>

size_t lzss_bound(size_t n);
size_t lzss_encode(const uint8_t *in, size_t n, uint8_t *out);
/* returns decoded bytes; (size_t)-1 on corrupt input / out overflow */
size_t lzss_decode(const uint8_t *in, size_t inN, uint8_t *out, size_t outN);

#endif
