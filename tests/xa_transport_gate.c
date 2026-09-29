#include "psx/dev/cdrom/cdrom.h"
#include "psx/perf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef puts
int cdrom_get_xa_samples(psx_cdrom_t*,void*,size_t);
static unsigned reads,decoded;
int psx_disc_read(psx_disc_t* disc,uint32_t lba,void* buffer) {
 (void)disc;++reads;uint8_t* s=buffer;memset(s,0,CD_SECTOR_SIZE);
 s[0x10]=1;s[0x11]=(lba%32==0)?2:3;s[0x12]=4;s[0x13]=4;
 for(int g=0;g<18;++g)memset(s+24+g*128+16,0x77,112);
 return TS_DATA;
}
static void probe(void* ctx,const psx_xa_probe_sector_t* p) {
 psx_cdrom_t* cd=ctx;assert(p->lba<=cd->lba || (cd->read_ongoing && p->lba==cd->lba+1 && cd->pending_lba==p->lba));++decoded;
}
int main(void) {
 static psx_cdrom_t cd;int16_t out[512*2];
 cd.mode=MODE_XA_ADPCM|MODE_XA_FILTER;cd.xa_playing=1;
 cd.xa_file=1;cd.xa_channel=2;cd.vol[0]=cd.vol[2]=0x80;
 g_psx_xa_probe=probe;g_psx_audio_pcm_probe_context=&cd;
 // Transport stays at sector 0 while the mixer exhausts that sector.
 for(int i=0;i<24;++i)cdrom_get_xa_samples(&cd,out,sizeof(out));
 assert(decoded==1 && cd.xa_playing && cd.xa_prefetch_state==2);
 assert(cd.xa_lba==1 && cd.xa_prefetch_next==33);
 unsigned cached_reads=reads;
 cdrom_get_xa_samples(&cd,out,sizeof(out));
 assert(reads==cached_reads && decoded==1);
 for(unsigned i=0;i<1024;++i)assert(out[i]==0);
 // A close sector is still blocked unless the drive has scheduled it.
 cd.lba=31;cdrom_get_xa_samples(&cd,out,sizeof(out));assert(decoded==1);
 cd.pending_lba=32;cd.read_ongoing=1;cd.state=CD_STATE_READ;
 cdrom_get_xa_samples(&cd,out,sizeof(out));
 assert(decoded==2 && reads==cached_reads && cd.xa_lba==33);
 // Pausing before a future sector is due discards its cached data.
 cdrom_get_xa_samples(&cd,out,sizeof(out));assert(cd.xa_prefetch_state==2);
 cd.xa_playing=0;cdrom_get_xa_samples(&cd,out,sizeof(out));assert(!cd.xa_prefetch_state);
 puts("XA bounded scheduled-sector playback tests passed");
}
