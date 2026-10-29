// SPDX-License-Identifier: GPL-3.0-or-later
// Independent GL command submission layer; no current DuckStation source used.
#include <glad.h>
#include "gl_worker.h"
#include <cstring>

SwanRender::Queue& SwanGLWorker::Commands() { static SwanRender::Queue queue; return queue; }

namespace {
struct glGetStringiTag {};
struct glUniform1fTag {};
struct glActiveTextureTag {};
struct glAttachShaderTag {};
struct glBindAttribLocationTag {};
struct glBindBufferTag {};
struct glBindBufferBaseTag {};
struct glBindBufferRangeTag {};
struct glBindFragDataLocationTag {};
struct glBindFragDataLocationIndexedTag {};
struct glBindFragDataLocationIndexedEXTTag {};
struct glBindFramebufferTag {};
struct glBindRenderbufferTag {};
struct glBindSamplerTag {};
struct glBindTextureTag {};
struct glBindVertexArrayTag {};
struct glBlendColorTag {};
struct glBlendEquationSeparateTag {};
struct glBlendFuncSeparateTag {};
struct glBlitFramebufferTag {};
struct glBufferDataTag {};
struct glBufferStorageTag {};
struct glBufferStorageEXTTag {};
struct glBufferSubDataTag {};
struct glCheckFramebufferStatusTag {};
struct glClearTag {};
struct glClearColorTag {};
struct glClearDepthTag {};
struct glClearDepthfTag {};
struct glClientWaitSyncTag {};
struct glColorMaskTag {};
struct glCompileShaderTag {};
struct glCopyImageSubDataTag {};
struct glCopyImageSubDataEXTTag {};
struct glCopyImageSubDataOESTag {};
struct glCreateProgramTag {};
struct glCreateShaderTag {};
struct glDebugMessageCallbackTag {};
struct glDebugMessageCallbackKHRTag {};
struct glDeleteBuffersTag {};
struct glDeleteFramebuffersTag {};
struct glDeleteProgramTag {};
struct glDeleteRenderbuffersTag {};
struct glDeleteSamplersTag {};
struct glDeleteShaderTag {};
struct glDeleteSyncTag {};
struct glDeleteTexturesTag {};
struct glDeleteVertexArraysTag {};
struct glDepthFuncTag {};
struct glDepthMaskTag {};
struct glDisableTag {};
struct glDrawArraysTag {};
struct glEnableTag {};
struct glEnableVertexAttribArrayTag {};
struct glFenceSyncTag {};
struct glFlushMappedBufferRangeTag {};
struct glFlushTag {};
struct glFramebufferRenderbufferTag {};
struct glFramebufferTexture2DTag {};
struct glGenBuffersTag {};
struct glGenFramebuffersTag {};
struct glGenRenderbuffersTag {};
struct glGenSamplersTag {};
struct glGenTexturesTag {};
struct glGenVertexArraysTag {};
struct glGetErrorTag {};
struct glGetInteger64vTag {};
struct glGetIntegervTag {};
struct glGetProgramBinaryTag {};
struct glGetProgramInfoLogTag {};
struct glGetProgramivTag {};
struct glGetShaderInfoLogTag {};
struct glGetShaderivTag {};
struct glGetStringTag {};
struct glGetUniformBlockIndexTag {};
struct glGetUniformLocationTag {};
struct glInvalidateFramebufferTag {};
struct glLinkProgramTag {};
struct glMapBufferRangeTag {};
struct glPixelStoreiTag {};
struct glProgramBinaryTag {};
struct glProgramParameteriTag {};
struct glReadPixelsTag {};
struct glRenderbufferStorageTag {};
struct glSamplerParameteriTag {};
struct glScissorTag {};
struct glShaderSourceTag {};
struct glTexBufferTag {};
struct glTexImage2DTag {};
struct glTexImage2DMultisampleTag {};
struct glTexParameteriTag {};
struct glTexStorage2DTag {};
struct glTexStorage2DMultisampleTag {};
struct glTexSubImage2DTag {};
struct glUniform1iTag {};
struct glUniform2fTag {};
struct glUniform4fTag {};
struct glUniformBlockBindingTag {};
struct glUnmapBufferTag {};
struct glUseProgramTag {};
struct glVertexAttribIPointerTag {};
struct glVertexAttribPointerTag {};
struct glViewportTag {};
}
void* SwanGLWorker::Procedure(const char* name, void* real) {
  if (!name || !real) return nullptr;
#define MATCH(name_) if (std::strcmp(name, #name_) == 0) \
  return reinterpret_cast<void*>(Wrap<name_##Tag>(reinterpret_cast<decltype(glad_##name_)>(real)));
  MATCH(glGetStringi)
  MATCH(glUniform1f)
  MATCH(glActiveTexture)
  MATCH(glAttachShader)
  MATCH(glBindAttribLocation)
  MATCH(glBindBuffer)
  MATCH(glBindBufferBase)
  MATCH(glBindBufferRange)
  MATCH(glBindFragDataLocation)
  MATCH(glBindFragDataLocationIndexed)
  MATCH(glBindFragDataLocationIndexedEXT)
  MATCH(glBindFramebuffer)
  MATCH(glBindRenderbuffer)
  MATCH(glBindSampler)
  MATCH(glBindTexture)
  MATCH(glBindVertexArray)
  MATCH(glBlendColor)
  MATCH(glBlendEquationSeparate)
  MATCH(glBlendFuncSeparate)
  MATCH(glBlitFramebuffer)
  MATCH(glBufferData)
  MATCH(glBufferStorage)
  MATCH(glBufferStorageEXT)
  MATCH(glBufferSubData)
  MATCH(glCheckFramebufferStatus)
  MATCH(glClear)
  MATCH(glClearColor)
  MATCH(glClearDepth)
  MATCH(glClearDepthf)
  MATCH(glClientWaitSync)
  MATCH(glColorMask)
  MATCH(glCompileShader)
  MATCH(glCopyImageSubData)
  MATCH(glCopyImageSubDataEXT)
  MATCH(glCopyImageSubDataOES)
  MATCH(glCreateProgram)
  MATCH(glCreateShader)
  MATCH(glDebugMessageCallback)
  MATCH(glDebugMessageCallbackKHR)
  MATCH(glDeleteBuffers)
  MATCH(glDeleteFramebuffers)
  MATCH(glDeleteProgram)
  MATCH(glDeleteRenderbuffers)
  MATCH(glDeleteSamplers)
  MATCH(glDeleteShader)
  MATCH(glDeleteSync)
  MATCH(glDeleteTextures)
  MATCH(glDeleteVertexArrays)
  MATCH(glDepthFunc)
  MATCH(glDepthMask)
  MATCH(glDisable)
  MATCH(glDrawArrays)
  MATCH(glEnable)
  MATCH(glEnableVertexAttribArray)
  MATCH(glFenceSync)
  MATCH(glFlushMappedBufferRange)
  MATCH(glFlush)
  MATCH(glFramebufferRenderbuffer)
  MATCH(glFramebufferTexture2D)
  MATCH(glGenBuffers)
  MATCH(glGenFramebuffers)
  MATCH(glGenRenderbuffers)
  MATCH(glGenSamplers)
  MATCH(glGenTextures)
  MATCH(glGenVertexArrays)
  MATCH(glGetError)
  MATCH(glGetInteger64v)
  MATCH(glGetIntegerv)
  MATCH(glGetProgramBinary)
  MATCH(glGetProgramInfoLog)
  MATCH(glGetProgramiv)
  MATCH(glGetShaderInfoLog)
  MATCH(glGetShaderiv)
  MATCH(glGetString)
  MATCH(glGetUniformBlockIndex)
  MATCH(glGetUniformLocation)
  MATCH(glInvalidateFramebuffer)
  MATCH(glLinkProgram)
  MATCH(glMapBufferRange)
  MATCH(glPixelStorei)
  MATCH(glProgramBinary)
  MATCH(glProgramParameteri)
  MATCH(glReadPixels)
  MATCH(glRenderbufferStorage)
  MATCH(glSamplerParameteri)
  MATCH(glScissor)
  MATCH(glShaderSource)
  MATCH(glTexBuffer)
  MATCH(glTexImage2D)
  MATCH(glTexImage2DMultisample)
  MATCH(glTexParameteri)
  MATCH(glTexStorage2D)
  MATCH(glTexStorage2DMultisample)
  MATCH(glTexSubImage2D)
  MATCH(glUniform1i)
  MATCH(glUniform2f)
  MATCH(glUniform4f)
  MATCH(glUniformBlockBinding)
  MATCH(glUnmapBuffer)
  MATCH(glUseProgram)
  MATCH(glVertexAttribIPointer)
  MATCH(glVertexAttribPointer)
  MATCH(glViewport)
#undef MATCH
  // GLAD probes many unused desktop/extension functions. Never give the core
  // an unwrapped driver entry point on the emulation thread. The coverage test
  // checks every GL call used by this core's OpenGL implementation.
  return nullptr;
}
