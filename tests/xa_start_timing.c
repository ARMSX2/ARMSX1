#include "psx/dev/cdrom/cdrom.h"
#include "psx/perf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef puts
int cdrom_get_xa_samples(psx_cdrom_t*,void*,size_t);
void cdrom_handle_read(psx_cdrom_t*);
static unsigned decoded;
int psx_disc_query(psx_disc_t* d,uint32_t lba) {(void)d;(void)lba;return TS_DATA;}
int psx_disc_read(psx_disc_t* d,uint32_t lba,void* buf) {
 (void)d; uint8_t* s=buf;memset(s,0,CD_SECTOR_SIZE);
 s[0x10]=1;s[0x11]=(lba%8==0)?1:2;s[0x12]=0x64;s[0x13]=1;
 for(int g=0;g<18;++g)memset(s+24+g*128+16,0x77,112);
 return TS_DATA;
}
static void probe(void* ctx,const psx_xa_probe_sector_t* p) {(void)ctx;(void)p;++decoded;}
int main(void) {
 psx_cdrom_t cd;memset(&cd,0,sizeof(cd));int16_t out[882*2];
 cd.mode=MODE_SPEED|MODE_XA_ADPCM|MODE_XA_FILTER;cd.xa_playing=1;
 cd.read_ongoing=1;cd.state=CD_STATE_READ;cd.xa_file=1;cd.xa_channel=1;
 cd.vol[0]=cd.vol[2]=0x80;cd.xa_start_phase=-1;
 g_psx_xa_probe=probe;
 cdrom_get_xa_samples(&cd,out,sizeof(out));assert(decoded==0);
 for(unsigned i=0;i<1764;++i)assert(out[i]==0);
 /* Other interleaved channels cannot start this stream. */
 cd.lba=1;cdrom_handle_read(&cd);assert(cd.xa_start_phase==-1);
 cd.pending_lba=8;cdrom_handle_read(&cd);assert(cd.xa_start_phase==1 && cd.xa_lba==8);
 /* Delivery at the end of a batch cannot retroactively play a whole batch. */
 cdrom_get_xa_samples(&cd,out,sizeof(out));assert(decoded==0 && cd.xa_start_phase==1);
 cd.xa_start_age_cycles=100*768;cdrom_get_xa_samples(&cd,out,sizeof(out));
 assert(decoded==1 && cd.xa_start_phase==0 && cd.xa_sample_index==100);
 for(unsigned i=0;i<(882-100)*2;++i)assert(out[i]==0);
 /* Exact delivery phase must remain continuous at PAL and NTSC batch sizes. */
 for(int batch=737;batch<=882;batch+=145) {
  memset(&cd,0,sizeof(cd));cd.mode=MODE_SPEED|MODE_XA_ADPCM|MODE_XA_FILTER;
  cd.xa_playing=1;cd.xa_start_phase=-1;cd.read_ongoing=1;cd.state=CD_STATE_READ;
  cd.xa_file=1;cd.xa_channel=1;cd.vol[0]=cd.vol[2]=0x80;
  uint64_t elapsed=0,next=400000,arrival=0;unsigned silent=0;
  for(int frame=0;frame<120;++frame) {
   elapsed+=(uint64_t)batch*768;
   while(next<=elapsed) {
    cdrom_handle_read(&cd);
    if(cd.xa_start_phase==1 && !arrival)arrival=next;
    next+=33868800/150;
   }
   if(cd.xa_start_phase==1)cd.xa_start_age_cycles=elapsed-arrival;
   cdrom_get_xa_samples(&cd,out,batch*4);
   if(frame>2)for(int i=0;i<batch;++i)if(!out[i*2]&&!out[i*2+1])++silent;
  }
  assert(silent==0);
 }
 puts("XA transport startup and PAL/NTSC continuity tests passed");
}
