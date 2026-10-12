// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <EGL/egl.h>
#include <GLES3/gl3.h>
namespace SwanGL {
#define SWAN_EGL_FUNCTIONS(X) X(eglChooseConfig) X(eglCreateContext) X(eglCreatePbufferSurface) X(eglCreateWindowSurface) X(eglDestroyContext) X(eglDestroySurface) X(eglGetDisplay) X(eglGetProcAddress) X(eglInitialize) X(eglMakeCurrent) X(eglQuerySurface) X(eglSwapBuffers) X(eglSwapInterval)
#define SWAN_GL_FUNCTIONS(X) X(glActiveTexture) X(glAttachShader) X(glBindFramebuffer) X(glBindRenderbuffer) X(glBindTexture) X(glBindVertexArray) X(glClear) X(glClearColor) X(glColorMask) X(glCompileShader) X(glCreateProgram) X(glCreateShader) X(glDeleteFramebuffers) X(glDeleteProgram) X(glDeleteRenderbuffers) X(glDeleteShader) X(glDeleteTextures) X(glDeleteVertexArrays) X(glDisable) X(glDrawArrays) X(glFramebufferRenderbuffer) X(glFramebufferTexture2D) X(glGenFramebuffers) X(glGenRenderbuffers) X(glGenTextures) X(glGenVertexArrays) X(glGetProgramiv) X(glGetShaderiv) X(glGetUniformLocation) X(glLinkProgram) X(glPixelStorei) X(glReadPixels) X(glRenderbufferStorage) X(glShaderSource) X(glTexImage2D) X(glTexParameteri) X(glUniform1i) X(glUniform1f) X(glUniform2f) X(glUseProgram) X(glViewport)
struct Api {
  decltype(&::glBlitFramebuffer) glBlitFramebuffer=nullptr;
  decltype(&::glBindSampler) glBindSampler=nullptr;
#define DECLARE(name) decltype(&::name) name=nullptr;
  SWAN_EGL_FUNCTIONS(DECLARE) SWAN_GL_FUNCTIONS(DECLARE)
#undef DECLARE
};
Api& Functions();
bool Load(bool angle);
bool IsAngle();
EGLDisplay Display();
}
#ifdef SWAN_GL_ROUTE
#define eglChooseConfig SwanGL::Functions().eglChooseConfig
#define eglCreateContext SwanGL::Functions().eglCreateContext
#define eglCreatePbufferSurface SwanGL::Functions().eglCreatePbufferSurface
#define eglCreateWindowSurface SwanGL::Functions().eglCreateWindowSurface
#define eglDestroyContext SwanGL::Functions().eglDestroyContext
#define eglDestroySurface SwanGL::Functions().eglDestroySurface
#define eglGetDisplay SwanGL::Functions().eglGetDisplay
#define eglGetProcAddress SwanGL::Functions().eglGetProcAddress
#define eglInitialize SwanGL::Functions().eglInitialize
#define eglMakeCurrent SwanGL::Functions().eglMakeCurrent
#define eglQuerySurface SwanGL::Functions().eglQuerySurface
#define eglSwapBuffers SwanGL::Functions().eglSwapBuffers
#define eglSwapInterval SwanGL::Functions().eglSwapInterval
#define glActiveTexture SwanGL::Functions().glActiveTexture
#define glAttachShader SwanGL::Functions().glAttachShader
#define glBindFramebuffer SwanGL::Functions().glBindFramebuffer
#define glBindRenderbuffer SwanGL::Functions().glBindRenderbuffer
#define glBindTexture SwanGL::Functions().glBindTexture
#define glBindVertexArray SwanGL::Functions().glBindVertexArray
#define glClear SwanGL::Functions().glClear
#define glClearColor SwanGL::Functions().glClearColor
#define glColorMask SwanGL::Functions().glColorMask
#define glCompileShader SwanGL::Functions().glCompileShader
#define glCreateProgram SwanGL::Functions().glCreateProgram
#define glCreateShader SwanGL::Functions().glCreateShader
#define glDeleteFramebuffers SwanGL::Functions().glDeleteFramebuffers
#define glDeleteProgram SwanGL::Functions().glDeleteProgram
#define glDeleteRenderbuffers SwanGL::Functions().glDeleteRenderbuffers
#define glDeleteShader SwanGL::Functions().glDeleteShader
#define glDeleteTextures SwanGL::Functions().glDeleteTextures
#define glDeleteVertexArrays SwanGL::Functions().glDeleteVertexArrays
#define glDisable SwanGL::Functions().glDisable
#define glDrawArrays SwanGL::Functions().glDrawArrays
#define glFramebufferRenderbuffer SwanGL::Functions().glFramebufferRenderbuffer
#define glFramebufferTexture2D SwanGL::Functions().glFramebufferTexture2D
#define glGenFramebuffers SwanGL::Functions().glGenFramebuffers
#define glGenRenderbuffers SwanGL::Functions().glGenRenderbuffers
#define glGenTextures SwanGL::Functions().glGenTextures
#define glGenVertexArrays SwanGL::Functions().glGenVertexArrays
#define glGetProgramiv SwanGL::Functions().glGetProgramiv
#define glGetShaderiv SwanGL::Functions().glGetShaderiv
#define glGetUniformLocation SwanGL::Functions().glGetUniformLocation
#define glLinkProgram SwanGL::Functions().glLinkProgram
#define glPixelStorei SwanGL::Functions().glPixelStorei
#define glReadPixels SwanGL::Functions().glReadPixels
#define glRenderbufferStorage SwanGL::Functions().glRenderbufferStorage
#define glShaderSource SwanGL::Functions().glShaderSource
#define glTexImage2D SwanGL::Functions().glTexImage2D
#define glTexParameteri SwanGL::Functions().glTexParameteri
#define glUniform1i SwanGL::Functions().glUniform1i
#define glUniform1f SwanGL::Functions().glUniform1f
#define glUniform2f SwanGL::Functions().glUniform2f
#define glUseProgram SwanGL::Functions().glUseProgram
#define glViewport SwanGL::Functions().glViewport
#define glBlitFramebuffer SwanGL::Functions().glBlitFramebuffer
#define glBindSampler SwanGL::Functions().glBindSampler
#endif
