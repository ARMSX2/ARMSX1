#pragma once
// Test-only GL declarations. Never add this directory to an Android include path.
using GLenum=unsigned;using GLuint=unsigned;using GLint=int;using GLsizei=int;
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
void glGetIntegerv(GLenum,GLint*);
void glGenQueries(GLsizei,GLuint*);
void glDeleteQueries(GLsizei,const GLuint*);
void glBeginQuery(GLenum,GLuint);
void glEndQuery(GLenum);
void glGetQueryObjectuiv(GLuint,GLenum,GLuint*);
