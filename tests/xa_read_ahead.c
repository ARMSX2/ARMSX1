#include "psx/dev/cdrom/cdrom.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#undef puts
int cdrom_get_xa_samples(psx_cdrom_t*,void*,size_t);
static unsigned reads;
int psx_disc_read(psx_disc_t* disc,uint32_t lba,void* buffer) {
    (void)disc; ++reads;
    usleep(300); /* Force the time budget to defer each 32-sector scan. */
    uint8_t* s = buffer; memset(s,0,CD_SECTOR_SIZE);
    s[0x10]=1; s[0x11]=(lba%32==0)?2:3; s[0x12]=4; s[0x13]=4;
    for(int g=0;g<18;++g) memset(s+24+g*128+16,0x77,112);
    return TS_DATA;
}
int main(void) {
    static psx_cdrom_t cd;
    int16_t out[737*2];
    cd.xa_playing=1; cd.mode=MODE_XA_ADPCM|MODE_XA_FILTER;
    cd.xa_file=1;cd.xa_channel=2;cd.vol[0]=cd.vol[2]=0x80;
    unsigned silent=0;
    for(int frame=0;frame<60;++frame) {
        /* The transport advances at 150 sectors/s; mixing follows the frame. */
        cd.lba = (uint32_t)(((uint64_t)(frame + 1) * 737 * 150) / 44100);
        if (cd.lba) --cd.lba;
        cd.pending_lba=cd.lba+1;
        cd.read_ongoing=1;
        cd.state=CD_STATE_READ;
        unsigned before=reads;
        assert(cdrom_get_xa_samples(&cd,out,sizeof(out))==1);
        assert(reads-before<=64);
        if(frame>0) for(unsigned i=0;i<737;++i)
            if(out[i*2]==0 && out[i*2+1]==0) ++silent;
    }
    assert(reads>128); /* Actually crossed multiple interleaved sectors. */
    assert(silent==0);
    /* Cached data must not survive a changed filter or stopped playback. */
    cd.xa_channel=7;
    cdrom_get_xa_samples(&cd,out,sizeof(out));
    assert(cd.xa_prefetch_channel==7 && cd.xa_prefetch_state!=2);
    cd.xa_playing=0;
    cdrom_get_xa_samples(&cd,out,sizeof(out));
    assert(cd.xa_prefetch_state==0);
    puts("XA slow-disc read-ahead continuity tests passed");
}
