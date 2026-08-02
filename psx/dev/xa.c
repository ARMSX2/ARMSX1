#include "xa.h"

#include <stdint.h>

void xa_decode_audio(uint8_t* src, uint16_t* dst) {
    // Not a XA sector
    if (src[XA_HDR_MODE] != 0x02)
        return;
    
    // uint8_t ci = src[XA_SHDR_CODINGINFO];
}