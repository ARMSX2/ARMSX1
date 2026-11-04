#pragma once
// Test-only declarations. Never include this directory in Android builds.
#include <cstdint>
#include <cstddef>
using GLenum=unsigned;using GLuint=unsigned;using GLint64=int64_t;using GLsizeiptr=ptrdiff_t;using GLintptr=ptrdiff_t;
constexpr GLenum GL_SHADER_STORAGE_BUFFER=0x90d2,GL_STREAM_DRAW=0x88e0,GL_NO_ERROR=0,GL_OUT_OF_MEMORY=0x505,GL_BUFFER_SIZE=0x8764;
void glBindBuffer(GLenum,GLuint);
void glBufferData(GLenum,GLsizeiptr,const void*,GLenum);
void glBufferSubData(GLenum,GLintptr,GLsizeiptr,const void*);
void glGetBufferParameteri64v(GLenum,GLenum,GLint64*);
GLenum glGetError();
