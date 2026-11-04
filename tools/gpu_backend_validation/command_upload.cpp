#include "gpu_command_upload.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {std::shared_ptr<std::vector<char>> store;GLenum error=0;unsigned replacements=0,writes=0;bool failAllocation=false;}
void glBindBuffer(GLenum target,GLuint buffer){assert(target==GL_SHADER_STORAGE_BUFFER&&buffer==1);}
void glBufferData(GLenum target,GLsizeiptr capacity,const void* data,GLenum usage){assert(target==GL_SHADER_STORAGE_BUFFER&&usage==GL_STREAM_DRAW&&data==nullptr);replacements++;if(failAllocation){error=GL_OUT_OF_MEMORY;return;}store=std::make_shared<std::vector<char>>(capacity);}
void glBufferSubData(GLenum target,GLintptr offset,GLsizeiptr bytes,const void* data){assert(target==GL_SHADER_STORAGE_BUFFER&&offset==0&&bytes<=ptrdiff_t(store->size()));std::memcpy(store->data(),data,bytes);writes++;}
void glGetBufferParameteri64v(GLenum target,GLenum name,GLint64* result){assert(target==GL_SHADER_STORAGE_BUFFER&&name==GL_BUFFER_SIZE);*result=store->size();}
GLenum glGetError(){auto result=error;error=0;return result;}
int main(){
    GpuCommandUpload uploader;store=std::make_shared<std::vector<char>>(8192);int a=123,b=456;
    uploader.upload(1,8192,4,&a);auto inFlight=store;uploader.upload(1,8192,4,&b);assert(store!=inFlight);int old=0,latest=0;std::memcpy(&old,inFlight->data(),4);std::memcpy(&latest,store->data(),4);assert(old==a&&latest==b);
    failAllocation=true;uploader.upload(1,8192,4,&a);unsigned attempts=replacements;uploader.upload(1,8192,4,&b);assert(replacements==attempts&&writes==4);
    bool rejected=false;try{uploader.upload(1,8192,8193,&b);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    GpuCommandUpload failed;store=std::make_shared<std::vector<char>>(2);rejected=false;try{failed.upload(1,8192,4,&b);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    std::puts("GLES command streaming passed: in-flight data isolation, allocation-failure fallback, bounded writes and undersized-store rejection.");
}
