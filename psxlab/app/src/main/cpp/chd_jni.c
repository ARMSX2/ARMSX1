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
