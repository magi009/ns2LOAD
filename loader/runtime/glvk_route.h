// Written by tools/glroute.py: the OpenGL ES functions runtime/nvn2.c calls, each with the port's own over Vulkan (runtime/glvk.c).
#pragma once
extern int nx2_vulkan;
void vk_glActiveTexture(GLenum texture);
#define glActiveTexture(...) (nx2_vulkan ? vk_glActiveTexture(__VA_ARGS__) : glActiveTexture(__VA_ARGS__))
void vk_glAttachShader(GLuint program, GLuint shader);
#define glAttachShader(...) (nx2_vulkan ? vk_glAttachShader(__VA_ARGS__) : glAttachShader(__VA_ARGS__))
void vk_glBindBuffer(GLenum target, GLuint buffer);
#define glBindBuffer(...) (nx2_vulkan ? vk_glBindBuffer(__VA_ARGS__) : glBindBuffer(__VA_ARGS__))
void vk_glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
#define glBindBufferBase(...) (nx2_vulkan ? vk_glBindBufferBase(__VA_ARGS__) : glBindBufferBase(__VA_ARGS__))
void vk_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
#define glBindBufferRange(...) (nx2_vulkan ? vk_glBindBufferRange(__VA_ARGS__) : glBindBufferRange(__VA_ARGS__))
void vk_glBindFramebuffer(GLenum target, GLuint framebuffer);
#define glBindFramebuffer(...) (nx2_vulkan ? vk_glBindFramebuffer(__VA_ARGS__) : glBindFramebuffer(__VA_ARGS__))
void vk_glBindSampler(GLuint unit, GLuint sampler);
#define glBindSampler(...) (nx2_vulkan ? vk_glBindSampler(__VA_ARGS__) : glBindSampler(__VA_ARGS__))
void vk_glBindTexture(GLenum target, GLuint texture);
#define glBindTexture(...) (nx2_vulkan ? vk_glBindTexture(__VA_ARGS__) : glBindTexture(__VA_ARGS__))
void vk_glBindVertexArray(GLuint array);
#define glBindVertexArray(...) (nx2_vulkan ? vk_glBindVertexArray(__VA_ARGS__) : glBindVertexArray(__VA_ARGS__))
void vk_glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
#define glBlendColor(...) (nx2_vulkan ? vk_glBlendColor(__VA_ARGS__) : glBlendColor(__VA_ARGS__))
void vk_glBlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha);
#define glBlendEquationSeparatei(...) (nx2_vulkan ? vk_glBlendEquationSeparatei(__VA_ARGS__) : glBlendEquationSeparatei(__VA_ARGS__))
void vk_glBlendFuncSeparatei(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
#define glBlendFuncSeparatei(...) (nx2_vulkan ? vk_glBlendFuncSeparatei(__VA_ARGS__) : glBlendFuncSeparatei(__VA_ARGS__))
void vk_glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
#define glBlitFramebuffer(...) (nx2_vulkan ? vk_glBlitFramebuffer(__VA_ARGS__) : glBlitFramebuffer(__VA_ARGS__))
void vk_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
#define glBufferData(...) (nx2_vulkan ? vk_glBufferData(__VA_ARGS__) : glBufferData(__VA_ARGS__))
void vk_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
#define glBufferSubData(...) (nx2_vulkan ? vk_glBufferSubData(__VA_ARGS__) : glBufferSubData(__VA_ARGS__))
GLenum vk_glCheckFramebufferStatus(GLenum target);
#define glCheckFramebufferStatus(...) (nx2_vulkan ? vk_glCheckFramebufferStatus(__VA_ARGS__) : glCheckFramebufferStatus(__VA_ARGS__))
void vk_glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value);
#define glClearBufferfv(...) (nx2_vulkan ? vk_glClearBufferfv(__VA_ARGS__) : glClearBufferfv(__VA_ARGS__))
void vk_glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint *value);
#define glClearBufferiv(...) (nx2_vulkan ? vk_glClearBufferiv(__VA_ARGS__) : glClearBufferiv(__VA_ARGS__))
void vk_glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
#define glColorMaski(...) (nx2_vulkan ? vk_glColorMaski(__VA_ARGS__) : glColorMaski(__VA_ARGS__))
void vk_glCompileShader(GLuint shader);
#define glCompileShader(...) (nx2_vulkan ? vk_glCompileShader(__VA_ARGS__) : glCompileShader(__VA_ARGS__))
void vk_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data);
#define glCompressedTexSubImage2D(...) (nx2_vulkan ? vk_glCompressedTexSubImage2D(__VA_ARGS__) : glCompressedTexSubImage2D(__VA_ARGS__))
void vk_glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void *data);
#define glCompressedTexSubImage3D(...) (nx2_vulkan ? vk_glCompressedTexSubImage3D(__VA_ARGS__) : glCompressedTexSubImage3D(__VA_ARGS__))
void vk_glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
#define glCopyImageSubData(...) (nx2_vulkan ? vk_glCopyImageSubData(__VA_ARGS__) : glCopyImageSubData(__VA_ARGS__))
GLuint vk_glCreateProgram(void);
#define glCreateProgram(...) (nx2_vulkan ? vk_glCreateProgram(__VA_ARGS__) : glCreateProgram(__VA_ARGS__))
GLuint vk_glCreateShader(GLenum type);
#define glCreateShader(...) (nx2_vulkan ? vk_glCreateShader(__VA_ARGS__) : glCreateShader(__VA_ARGS__))
void vk_glCullFace(GLenum mode);
#define glCullFace(...) (nx2_vulkan ? vk_glCullFace(__VA_ARGS__) : glCullFace(__VA_ARGS__))
void vk_glDeleteBuffers(GLsizei n, const GLuint *buffers);
#define glDeleteBuffers(...) (nx2_vulkan ? vk_glDeleteBuffers(__VA_ARGS__) : glDeleteBuffers(__VA_ARGS__))
void vk_glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
#define glDeleteFramebuffers(...) (nx2_vulkan ? vk_glDeleteFramebuffers(__VA_ARGS__) : glDeleteFramebuffers(__VA_ARGS__))
void vk_glDeleteProgram(GLuint program);
#define glDeleteProgram(...) (nx2_vulkan ? vk_glDeleteProgram(__VA_ARGS__) : glDeleteProgram(__VA_ARGS__))
void vk_glDeleteShader(GLuint shader);
#define glDeleteShader(...) (nx2_vulkan ? vk_glDeleteShader(__VA_ARGS__) : glDeleteShader(__VA_ARGS__))
void vk_glDeleteTextures(GLsizei n, const GLuint *textures);
#define glDeleteTextures(...) (nx2_vulkan ? vk_glDeleteTextures(__VA_ARGS__) : glDeleteTextures(__VA_ARGS__))
void vk_glDepthFunc(GLenum func);
#define glDepthFunc(...) (nx2_vulkan ? vk_glDepthFunc(__VA_ARGS__) : glDepthFunc(__VA_ARGS__))
void vk_glDepthMask(GLboolean flag);
#define glDepthMask(...) (nx2_vulkan ? vk_glDepthMask(__VA_ARGS__) : glDepthMask(__VA_ARGS__))
void vk_glDepthRangef(GLfloat n, GLfloat f);
#define glDepthRangef(...) (nx2_vulkan ? vk_glDepthRangef(__VA_ARGS__) : glDepthRangef(__VA_ARGS__))
void vk_glDisable(GLenum cap);
#define glDisable(...) (nx2_vulkan ? vk_glDisable(__VA_ARGS__) : glDisable(__VA_ARGS__))
void vk_glDisableVertexAttribArray(GLuint index);
#define glDisableVertexAttribArray(...) (nx2_vulkan ? vk_glDisableVertexAttribArray(__VA_ARGS__) : glDisableVertexAttribArray(__VA_ARGS__))
void vk_glDisablei(GLenum target, GLuint index);
#define glDisablei(...) (nx2_vulkan ? vk_glDisablei(__VA_ARGS__) : glDisablei(__VA_ARGS__))
void vk_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
#define glDrawArraysInstanced(...) (nx2_vulkan ? vk_glDrawArraysInstanced(__VA_ARGS__) : glDrawArraysInstanced(__VA_ARGS__))
void vk_glDrawBuffers(GLsizei n, const GLenum *bufs);
#define glDrawBuffers(...) (nx2_vulkan ? vk_glDrawBuffers(__VA_ARGS__) : glDrawBuffers(__VA_ARGS__))
void vk_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
#define glDrawElements(...) (nx2_vulkan ? vk_glDrawElements(__VA_ARGS__) : glDrawElements(__VA_ARGS__))
void vk_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
#define glDrawElementsInstanced(...) (nx2_vulkan ? vk_glDrawElementsInstanced(__VA_ARGS__) : glDrawElementsInstanced(__VA_ARGS__))
void vk_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex);
#define glDrawElementsInstancedBaseVertex(...) (nx2_vulkan ? vk_glDrawElementsInstancedBaseVertex(__VA_ARGS__) : glDrawElementsInstancedBaseVertex(__VA_ARGS__))
void vk_glEnable(GLenum cap);
#define glEnable(...) (nx2_vulkan ? vk_glEnable(__VA_ARGS__) : glEnable(__VA_ARGS__))
void vk_glEnableVertexAttribArray(GLuint index);
#define glEnableVertexAttribArray(...) (nx2_vulkan ? vk_glEnableVertexAttribArray(__VA_ARGS__) : glEnableVertexAttribArray(__VA_ARGS__))
void vk_glEnablei(GLenum target, GLuint index);
#define glEnablei(...) (nx2_vulkan ? vk_glEnablei(__VA_ARGS__) : glEnablei(__VA_ARGS__))
void vk_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
#define glFramebufferTexture2D(...) (nx2_vulkan ? vk_glFramebufferTexture2D(__VA_ARGS__) : glFramebufferTexture2D(__VA_ARGS__))
void vk_glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
#define glFramebufferTextureLayer(...) (nx2_vulkan ? vk_glFramebufferTextureLayer(__VA_ARGS__) : glFramebufferTextureLayer(__VA_ARGS__))
void vk_glFrontFace(GLenum mode);
#define glFrontFace(...) (nx2_vulkan ? vk_glFrontFace(__VA_ARGS__) : glFrontFace(__VA_ARGS__))
void vk_glGenBuffers(GLsizei n, GLuint *buffers);
#define glGenBuffers(...) (nx2_vulkan ? vk_glGenBuffers(__VA_ARGS__) : glGenBuffers(__VA_ARGS__))
void vk_glGenFramebuffers(GLsizei n, GLuint *framebuffers);
#define glGenFramebuffers(...) (nx2_vulkan ? vk_glGenFramebuffers(__VA_ARGS__) : glGenFramebuffers(__VA_ARGS__))
void vk_glGenSamplers(GLsizei count, GLuint *samplers);
#define glGenSamplers(...) (nx2_vulkan ? vk_glGenSamplers(__VA_ARGS__) : glGenSamplers(__VA_ARGS__))
void vk_glGenTextures(GLsizei n, GLuint *textures);
#define glGenTextures(...) (nx2_vulkan ? vk_glGenTextures(__VA_ARGS__) : glGenTextures(__VA_ARGS__))
void vk_glGenVertexArrays(GLsizei n, GLuint *arrays);
#define glGenVertexArrays(...) (nx2_vulkan ? vk_glGenVertexArrays(__VA_ARGS__) : glGenVertexArrays(__VA_ARGS__))
void vk_glGetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei *length, GLchar *uniformBlockName);
#define glGetActiveUniformBlockName(...) (nx2_vulkan ? vk_glGetActiveUniformBlockName(__VA_ARGS__) : glGetActiveUniformBlockName(__VA_ARGS__))
void vk_glGetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint *params);
#define glGetActiveUniformBlockiv(...) (nx2_vulkan ? vk_glGetActiveUniformBlockiv(__VA_ARGS__) : glGetActiveUniformBlockiv(__VA_ARGS__))
void vk_glGetActiveUniformsiv(GLuint program, GLsizei uniformCount, const GLuint *uniformIndices, GLenum pname, GLint *params);
#define glGetActiveUniformsiv(...) (nx2_vulkan ? vk_glGetActiveUniformsiv(__VA_ARGS__) : glGetActiveUniformsiv(__VA_ARGS__))
void vk_glGetBooleanv(GLenum pname, GLboolean *data);
#define glGetBooleanv(...) (nx2_vulkan ? vk_glGetBooleanv(__VA_ARGS__) : glGetBooleanv(__VA_ARGS__))
GLenum vk_glGetError(void);
#define glGetError(...) (nx2_vulkan ? vk_glGetError(__VA_ARGS__) : glGetError(__VA_ARGS__))
void vk_glGetIntegerv(GLenum pname, GLint *data);
#define glGetIntegerv(...) (nx2_vulkan ? vk_glGetIntegerv(__VA_ARGS__) : glGetIntegerv(__VA_ARGS__))
void vk_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
#define glGetProgramInfoLog(...) (nx2_vulkan ? vk_glGetProgramInfoLog(__VA_ARGS__) : glGetProgramInfoLog(__VA_ARGS__))
void vk_glGetProgramInterfaceiv(GLuint program, GLenum programInterface, GLenum pname, GLint *params);
#define glGetProgramInterfaceiv(...) (nx2_vulkan ? vk_glGetProgramInterfaceiv(__VA_ARGS__) : glGetProgramInterfaceiv(__VA_ARGS__))
void vk_glGetProgramiv(GLuint program, GLenum pname, GLint *params);
#define glGetProgramiv(...) (nx2_vulkan ? vk_glGetProgramiv(__VA_ARGS__) : glGetProgramiv(__VA_ARGS__))
void vk_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
#define glGetShaderInfoLog(...) (nx2_vulkan ? vk_glGetShaderInfoLog(__VA_ARGS__) : glGetShaderInfoLog(__VA_ARGS__))
void vk_glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
#define glGetShaderiv(...) (nx2_vulkan ? vk_glGetShaderiv(__VA_ARGS__) : glGetShaderiv(__VA_ARGS__))
const GLubyte * vk_glGetString(GLenum name);
#define glGetString(...) (nx2_vulkan ? vk_glGetString(__VA_ARGS__) : glGetString(__VA_ARGS__))
void vk_glGetUniformIndices(GLuint program, GLsizei uniformCount, const GLchar *const*uniformNames, GLuint *uniformIndices);
#define glGetUniformIndices(...) (nx2_vulkan ? vk_glGetUniformIndices(__VA_ARGS__) : glGetUniformIndices(__VA_ARGS__))
GLint vk_glGetUniformLocation(GLuint program, const GLchar *name);
#define glGetUniformLocation(...) (nx2_vulkan ? vk_glGetUniformLocation(__VA_ARGS__) : glGetUniformLocation(__VA_ARGS__))
GLboolean vk_glIsEnabled(GLenum cap);
#define glIsEnabled(...) (nx2_vulkan ? vk_glIsEnabled(__VA_ARGS__) : glIsEnabled(__VA_ARGS__))
void vk_glLinkProgram(GLuint program);
#define glLinkProgram(...) (nx2_vulkan ? vk_glLinkProgram(__VA_ARGS__) : glLinkProgram(__VA_ARGS__))
void vk_glPixelStorei(GLenum pname, GLint param);
#define glPixelStorei(...) (nx2_vulkan ? vk_glPixelStorei(__VA_ARGS__) : glPixelStorei(__VA_ARGS__))
void vk_glPolygonOffset(GLfloat factor, GLfloat units);
#define glPolygonOffset(...) (nx2_vulkan ? vk_glPolygonOffset(__VA_ARGS__) : glPolygonOffset(__VA_ARGS__))
void vk_glReadBuffer(GLenum src);
#define glReadBuffer(...) (nx2_vulkan ? vk_glReadBuffer(__VA_ARGS__) : glReadBuffer(__VA_ARGS__))
void vk_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
#define glReadPixels(...) (nx2_vulkan ? vk_glReadPixels(__VA_ARGS__) : glReadPixels(__VA_ARGS__))
void vk_glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param);
#define glSamplerParameterf(...) (nx2_vulkan ? vk_glSamplerParameterf(__VA_ARGS__) : glSamplerParameterf(__VA_ARGS__))
void vk_glSamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat *param);
#define glSamplerParameterfv(...) (nx2_vulkan ? vk_glSamplerParameterfv(__VA_ARGS__) : glSamplerParameterfv(__VA_ARGS__))
void vk_glSamplerParameteri(GLuint sampler, GLenum pname, GLint param);
#define glSamplerParameteri(...) (nx2_vulkan ? vk_glSamplerParameteri(__VA_ARGS__) : glSamplerParameteri(__VA_ARGS__))
void vk_glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
#define glScissor(...) (nx2_vulkan ? vk_glScissor(__VA_ARGS__) : glScissor(__VA_ARGS__))
void vk_glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
#define glShaderSource(...) (nx2_vulkan ? vk_glShaderSource(__VA_ARGS__) : glShaderSource(__VA_ARGS__))
void vk_glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
#define glStencilFuncSeparate(...) (nx2_vulkan ? vk_glStencilFuncSeparate(__VA_ARGS__) : glStencilFuncSeparate(__VA_ARGS__))
void vk_glStencilMask(GLuint mask);
#define glStencilMask(...) (nx2_vulkan ? vk_glStencilMask(__VA_ARGS__) : glStencilMask(__VA_ARGS__))
void vk_glStencilMaskSeparate(GLenum face, GLuint mask);
#define glStencilMaskSeparate(...) (nx2_vulkan ? vk_glStencilMaskSeparate(__VA_ARGS__) : glStencilMaskSeparate(__VA_ARGS__))
void vk_glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
#define glStencilOpSeparate(...) (nx2_vulkan ? vk_glStencilOpSeparate(__VA_ARGS__) : glStencilOpSeparate(__VA_ARGS__))
void vk_glTexParameteri(GLenum target, GLenum pname, GLint param);
#define glTexParameteri(...) (nx2_vulkan ? vk_glTexParameteri(__VA_ARGS__) : glTexParameteri(__VA_ARGS__))
void vk_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
#define glTexStorage2D(...) (nx2_vulkan ? vk_glTexStorage2D(__VA_ARGS__) : glTexStorage2D(__VA_ARGS__))
void vk_glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
#define glTexStorage3D(...) (nx2_vulkan ? vk_glTexStorage3D(__VA_ARGS__) : glTexStorage3D(__VA_ARGS__))
void vk_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
#define glTexSubImage2D(...) (nx2_vulkan ? vk_glTexSubImage2D(__VA_ARGS__) : glTexSubImage2D(__VA_ARGS__))
void vk_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels);
#define glTexSubImage3D(...) (nx2_vulkan ? vk_glTexSubImage3D(__VA_ARGS__) : glTexSubImage3D(__VA_ARGS__))
void vk_glUniform1i(GLint location, GLint v0);
#define glUniform1i(...) (nx2_vulkan ? vk_glUniform1i(__VA_ARGS__) : glUniform1i(__VA_ARGS__))
void vk_glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
#define glUniformBlockBinding(...) (nx2_vulkan ? vk_glUniformBlockBinding(__VA_ARGS__) : glUniformBlockBinding(__VA_ARGS__))
void vk_glUseProgram(GLuint program);
#define glUseProgram(...) (nx2_vulkan ? vk_glUseProgram(__VA_ARGS__) : glUseProgram(__VA_ARGS__))
void vk_glVertexAttribDivisor(GLuint index, GLuint divisor);
#define glVertexAttribDivisor(...) (nx2_vulkan ? vk_glVertexAttribDivisor(__VA_ARGS__) : glVertexAttribDivisor(__VA_ARGS__))
void vk_glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
#define glVertexAttribI4ui(...) (nx2_vulkan ? vk_glVertexAttribI4ui(__VA_ARGS__) : glVertexAttribI4ui(__VA_ARGS__))
void vk_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer);
#define glVertexAttribIPointer(...) (nx2_vulkan ? vk_glVertexAttribIPointer(__VA_ARGS__) : glVertexAttribIPointer(__VA_ARGS__))
void vk_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
#define glVertexAttribPointer(...) (nx2_vulkan ? vk_glVertexAttribPointer(__VA_ARGS__) : glVertexAttribPointer(__VA_ARGS__))
void vk_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
#define glViewport(...) (nx2_vulkan ? vk_glViewport(__VA_ARGS__) : glViewport(__VA_ARGS__))
