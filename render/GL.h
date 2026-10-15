#pragma once

// Pulls in OpenGL 3.2+ core declarations for each platform.
//
// macOS ships the core-profile headers. On Windows, opengl32 only exports
// GL 1.1, so the newer entry points are loaded at runtime by gl::loadFunctions()
// (see GL.cpp). Call it once after a context is current.

#if defined(__APPLE__)
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl3.h>

namespace gl {
inline bool loadFunctions(void* (*)(const char*)) { return true; }
} // namespace gl

#else

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <GL/gl.h>

#include <cstddef>

#ifndef APIENTRY
#define APIENTRY
#endif

using GLchar = char;

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE0 0x84C0
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_R8 0x8229
#define GL_RED 0x1903
#define GL_TEXTURE1 0x84C1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6

// X-macro list of every post-1.1 function the renderer uses.
#define H89_GL_FUNCTIONS(X)                                                              \
    X(GLuint, glCreateShader, (GLenum type))                                             \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar* const* src, const GLint* len)) \
    X(void, glCompileShader, (GLuint s))                                                 \
    X(void, glGetShaderiv, (GLuint s, GLenum p, GLint* v))                               \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei* len, GLchar* log))      \
    X(void, glDeleteShader, (GLuint s))                                                  \
    X(GLuint, glCreateProgram, (void))                                                   \
    X(void, glAttachShader, (GLuint p, GLuint s))                                        \
    X(void, glLinkProgram, (GLuint p))                                                   \
    X(void, glGetProgramiv, (GLuint p, GLenum n, GLint* v))                              \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei* len, GLchar* log))     \
    X(void, glUseProgram, (GLuint p))                                                    \
    X(void, glDeleteProgram, (GLuint p))                                                 \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar* name))                       \
    X(void, glUniform1i, (GLint loc, GLint v))                                           \
    X(void, glUniform1f, (GLint loc, GLfloat v))                                         \
    X(void, glUniform2f, (GLint loc, GLfloat a, GLfloat b))                              \
    X(void, glUniform3f, (GLint loc, GLfloat a, GLfloat b, GLfloat c))                   \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* a))                                   \
    X(void, glBindVertexArray, (GLuint a))                                               \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* a))                          \
    X(void, glActiveTexture, (GLenum t))                                                 \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* f))                                   \
    X(void, glBindFramebuffer, (GLenum target, GLuint f))                                \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum tt, GLuint t, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                 \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* f))

#define H89_GL_DECLARE(ret, name, args)                                                  \
    using PFN_##name = ret(APIENTRY*) args;                                              \
    extern PFN_##name name;
H89_GL_FUNCTIONS(H89_GL_DECLARE)
#undef H89_GL_DECLARE

namespace gl {
// `getProc` is e.g. glfwGetProcAddress or wglGetProcAddress.
bool loadFunctions(void* (*getProc)(const char*));
} // namespace gl

#endif
