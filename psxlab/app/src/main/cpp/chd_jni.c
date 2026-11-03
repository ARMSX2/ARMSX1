#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <libchdr/chd.h>

typedef struct { FILE *file; chd_file *chd; unsigned char *buffer; uint32_t cached; } reader;
static void fail(JNIEnv *env, const char *message) {
    (*env)->ThrowNew(env, (*env)->FindClass(env,"java/io/IOException"),message);
}
JNIEXPORT jlong JNICALL Java_dev_psxlab_ChdSource_open(JNIEnv *env,jclass cls,jint fd) {
    (void)cls;
    reader *r=calloc(1,sizeof(*r));
    if(!r){fail(env,"CHD reader allocation failed");return 0;}
    int copy=dup(fd);
    if(copy<0){free(r);fail(env,"Cannot duplicate disc descriptor");return 0;}
    r->file=fdopen(copy,"rb");
    if(!r->file){close(copy);free(r);fail(env,"Cannot open disc stream");return 0;}
    chd_error error=chd_open_file(r->file,CHD_OPEN_READ,NULL,&r->chd);
    if(error!=CHDERR_NONE){fclose(r->file);free(r);fail(env,chd_error_string(error));return 0;}
    const chd_header *h=chd_get_header(r->chd);
    if(h->hunkbytes==0 || h->hunkbytes>16*1024*1024 || h->unitbytes!=2448){
        chd_close(r->chd);fclose(r->file);free(r);fail(env,"Only CD CHDs with 2448-byte units are supported");return 0;
    }
    r->buffer=malloc(h->hunkbytes);r->cached=UINT32_MAX;
    if(!r->buffer){chd_close(r->chd);fclose(r->file);free(r);fail(env,"CHD hunk allocation failed");return 0;}
    return (jlong)(intptr_t)r;
}
JNIEXPORT void JNICALL Java_dev_psxlab_ChdSource_readNative(JNIEnv *env,jclass cls,jlong handle,jlong offset,jbyteArray output) {
    (void)cls;reader *r=(reader*)(intptr_t)handle;
    const chd_header *h=chd_get_header(r->chd);
    jsize length=(*env)->GetArrayLength(env,output);
    if(offset<0 || (uint64_t)offset>h->logicalbytes || (uint64_t)length>h->logicalbytes-(uint64_t)offset){fail(env,"CHD read outside disc");return;}
    jsize done=0;
    while(done<length){
        uint64_t at=(uint64_t)offset+(uint64_t)done;
        uint32_t index=(uint32_t)(at/h->hunkbytes),within=(uint32_t)(at%h->hunkbytes);
        if(r->cached!=index){
            chd_error error=chd_read(r->chd,index,r->buffer);
            if(error!=CHDERR_NONE){fail(env,chd_error_string(error));return;}
            r->cached=index;
        }
        uint32_t n=h->hunkbytes-within;if(n>(uint32_t)(length-done))n=(uint32_t)(length-done);
        (*env)->SetByteArrayRegion(env,output,done,(jsize)n,(jbyte*)(r->buffer+within));
        if((*env)->ExceptionCheck(env))return;
        done+=(jsize)n;
    }
}
JNIEXPORT void JNICALL Java_dev_psxlab_ChdSource_closeNative(JNIEnv *env,jclass cls,jlong handle){
    (void)env;(void)cls;reader *r=(reader*)(intptr_t)handle;
    chd_close(r->chd);fclose(r->file);free(r->buffer);free(r);
}
JNIEXPORT jlong JNICALL Java_dev_psxlab_ChdSource_lengthNative(JNIEnv *env,jclass cls,jlong handle){
    (void)env;(void)cls;reader *r=(reader*)(intptr_t)handle;return (jlong)chd_get_header(r->chd)->logicalbytes;
}

/* Track table from CHT2 (or older CHTR) metadata: {audio, frames, pregap, pregap stored, postgap} per track.
 * Empty when the CHD has no CD track metadata; the caller then keeps the flat single-track layout. */
JNIEXPORT jintArray JNICALL Java_dev_psxlab_ChdSource_tracksNative(JNIEnv *env,jclass cls,jlong handle){
    (void)cls;reader *r=(reader*)(intptr_t)handle;
    jint values[99*5];int count=0;char meta[256];
    for(uint32_t index=0;index<99;index++){
        uint32_t length=0,tag=0;uint8_t flags=0;memset(meta,0,sizeof(meta));
        int track=0,frames=0,pregap=0,postgap=0;char type[64]={0},subtype[64]={0},pgtype[64]={0},pgsub[64]={0};
        if(chd_get_metadata(r->chd,CDROM_TRACK_METADATA2_TAG,index,meta,sizeof(meta)-1,&length,&tag,&flags)==CHDERR_NONE){
            if(sscanf(meta,CDROM_TRACK_METADATA2_FORMAT,&track,type,subtype,&frames,&pregap,pgtype,pgsub,&postgap)!=8)break;
        }else if(chd_get_metadata(r->chd,CDROM_TRACK_METADATA_TAG,index,meta,sizeof(meta)-1,&length,&tag,&flags)==CHDERR_NONE){
            if(sscanf(meta,CDROM_TRACK_METADATA_FORMAT,&track,type,subtype,&frames)!=4)break;
        }else break;
        if(track!=(int)index+1||frames<=0)break;
        jint *v=values+count*5;v[0]=strcmp(type,"AUDIO")==0;v[1]=frames;v[2]=pregap;v[3]=pgtype[0]=='V';v[4]=postgap;count++;
    }
    jintArray out=(*env)->NewIntArray(env,count*5);
    if(out&&count)(*env)->SetIntArrayRegion(env,out,0,count*5,values);
    return out;
}
