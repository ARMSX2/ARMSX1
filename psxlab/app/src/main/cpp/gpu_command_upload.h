#pragma once
#include <GLES3/gl31.h>
#include <stdexcept>

// GLES 3.1 baseline APIs only. Replacing the store lets the driver retain old
// command data for in-flight draws instead of serializing every offset-zero write.
// Keep the established subdata path if the optional replacement allocation fails.
class GpuCommandUpload {
    bool replaceStore=true;
public:
    void upload(GLuint buffer,GLsizeiptr capacity,GLsizeiptr bytes,const void* data){
        if(bytes<0||bytes>capacity)throw std::runtime_error("GPU command upload exceeds buffer capacity");
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffer);
        if(replaceStore){
            glBufferData(GL_SHADER_STORAGE_BUFFER,capacity,nullptr,GL_STREAM_DRAW);
            GLenum error=glGetError();
            if(error==GL_OUT_OF_MEMORY){
                replaceStore=false;
                GLint64 available=0;glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER,GL_BUFFER_SIZE,&available);
                if(available<bytes)throw std::runtime_error("GPU command upload allocation failed");
            }else if(error!=GL_NO_ERROR)throw std::runtime_error("GPU command buffer replacement failed");
        }
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,bytes,data);
    }
};
