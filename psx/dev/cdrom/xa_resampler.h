#ifndef ARMSX_XA_RESAMPLER_H
#define ARMSX_XA_RESAMPLER_H
#include <stdint.h>
#include <stddef.h>
/* Measured XA filter coefficients documented by psx-spx:
   https://psx-spx.consoledev.net/psx-spx.pdf (CDROM XA Audio ADPCM Compression).
   Rows are delays 1..29; columns are the seven output phases. */
static const int16_t xa_filter[29][7] = {
    {0, 0, 0, 0, -1, 2, -5},
    {0, 0, 0, -1, 3, -8, 17},
    {0, 0, -1, 3, -8, 16, -35},
    {0, -2, 3, -8, 17, -35, 70},
    {0, 0, -2, 6, -16, 43, -23},
    {-2, 3, -5, 5, 10, 26, -68},
    {10, -19, 31, -27, 107, -235, 347},
    {-34, 60, -74, 166, -365, 635, -839},
    {65, -75, 179, -424, 848, -1352, 2062},
    {-84, 162, -402, 882, -1571, 2810, -4681},
    {52, -227, 689, -1471, 3021, -5882, 15367},
    {9, 306, -926, 2488, -6016, 21472, 21472},
    {-266, -67, 1272, -4532, 26516, 15367, -5882},
    {1024, -615, -1446, 29883, 9036, -4681, 2810},
    {-2680, 3229, 31033, 3229, -2680, 2062, -1352},
    {9036, 29883, -1446, -615, 1024, -839, 635},
    {26516, -4532, 1272, -67, -266, 347, -235},
    {-6016, 2488, -926, 306, 9, -68, 26},
    {3021, -1471, 689, -227, 52, -23, 43},
    {-1571, 882, -402, 162, -84, 70, -35},
    {848, -424, 179, -75, 65, -35, 16},
    {-365, 166, -74, 60, -34, 17, -8},
    {107, -27, 31, -19, 10, -5, 2},
    {10, 5, -5, 3, -1, 0, 0},
    {-16, 6, -2, 0, 0, 0, 0},
    {17, -8, 3, -2, 1, 0, 0},
    {-8, 3, -1, 0, 0, 0, 0},
    {3, -1, 0, 0, 0, 0, 0},
    {-1, 0, 0, 0, 0, 0, 0},
};
/* One sector always contains a multiple of six input samples and 32 history
   samples, so the phase/ring position starts and ends at zero. At 18.9 kHz
   each decoded input is repeated twice before the 37.8 -> 44.1 kHz filter. */
static inline size_t xa_resample_filtered(const int16_t* src, size_t count,
                                         int half_rate, int16_t history[32], int16_t* dst) {
    unsigned position = 0, six = 0;
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        for (int repeat = 0; repeat <= half_rate; ++repeat) {
            history[position] = src[i];
            position = (position + 1) & 31;
            if (++six != 6) continue;
            six = 0;
            for (unsigned phase = 0; phase < 7; ++phase) {
                int32_t sum = 0;
                for (unsigned tap = 0; tap < 29; ++tap)
                    sum += ((int32_t)history[(position - 1 - tap) & 31] * xa_filter[tap][phase]) >> 15;
                dst[written++] = (int16_t)(sum < -32768 ? -32768 : sum > 32767 ? 32767 : sum);
            }
        }
    }
    return written;
}
#endif
