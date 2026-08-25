/* Definitions for the OpenGL entry points a libretro core calls DIRECTLY, for a console that
 * has no OpenGL context driver in its frontend.
 *
 * ⚠ WHY THIS IS NOT A HACK, AND WHERE ITS LIMIT IS.
 *
 * A core like mupen64plus-next carries TWO renderers: GLideN64 over OpenGL, and paraLLEl-RDP
 * over Vulkan. Only the Vulkan one can run here - gfx/drivers_context has orbis_vk_ctx.c and
 * nothing for RETRO_HW_CONTEXT_OPENGL - but the GL renderer is compiled unconditionally by its
 * Makefile, so its ~200 translation units join the link and drag their GL calls in with them.
 *
 * MOST of a core's GL calls do not appear here, because libretro's glsm layer turns them into
 * macros over function pointers it loads at run time (libretro-common/include/glsm/glsmsym.h).
 * Those need no definition at link time and are never reached if the renderer is never selected.
 * The 35 below are the ones GLideN64 calls under names glsm does not intercept, and each one is
 * a genuine undefined symbol that stops the module from linking.
 *
 * ⚠ THESE MUST NEVER BE CALLED, and that is enforced elsewhere, not here. The core's RDP plugin
 * defaults to paraLLEl on this platform; if a user forces GLideN64 anyway, reaching these means
 * a renderer is drawing into nothing. They are quiet rather than fatal because a core is not the
 * right place to abort a process, but silence is the reason this file logs the first call.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float         GLfloat;
typedef float         GLclampf;
typedef double        GLclampd;
typedef double        GLdouble;
typedef unsigned int  GLbitfield;
typedef ptrdiff_t     GLintptr;
typedef ptrdiff_t     GLsizeiptr;

/* One line, once, whoever gets here first. Not thread-safe by design: the worst case is two
 * lines in the log, and taking a lock in a stub that must never run is worse than that. */
static int orbis_gl_warned;
static void orbis_gl_once(const char *fn)
{
   if (orbis_gl_warned)
      return;
   orbis_gl_warned = 1;
   fprintf(stderr, "[orbis] OpenGL entry point called (%s) - this build has no GL context. "
                   "Select the Vulkan renderer.\n", fn);
}

#define GL_STUB_VOID(name, ...) \
   void name(__VA_ARGS__) { orbis_gl_once(#name); }

GL_STUB_VOID(glBindTexture,    GLenum t, GLuint x)
GL_STUB_VOID(glBlendFunc,      GLenum s, GLenum d)
GL_STUB_VOID(glClear,          GLbitfield m)
GL_STUB_VOID(glClearColor,     GLclampf r, GLclampf g, GLclampf b, GLclampf a)
GL_STUB_VOID(glClearDepth,     GLclampd d)
GL_STUB_VOID(glColorMask,      GLboolean r, GLboolean g, GLboolean b, GLboolean a)
GL_STUB_VOID(glCullFace,       GLenum m)
GL_STUB_VOID(glDeleteTextures, GLsizei n, const GLuint *t)
GL_STUB_VOID(glDepthFunc,      GLenum f)
GL_STUB_VOID(glDepthMask,      GLboolean f)
GL_STUB_VOID(glDepthRange,     GLclampd n, GLclampd f)
GL_STUB_VOID(glDisable,        GLenum c)
GL_STUB_VOID(glDrawArrays,     GLenum m, GLint f, GLsizei c)
GL_STUB_VOID(glDrawElements,   GLenum m, GLsizei c, GLenum t, const void *i)
GL_STUB_VOID(glEnable,         GLenum c)
GL_STUB_VOID(glFinish,         void)
GL_STUB_VOID(glFrontFace,      GLenum m)
GL_STUB_VOID(glGenTextures,    GLsizei n, GLuint *t)
GL_STUB_VOID(glLineWidth,      GLfloat w)
GL_STUB_VOID(glPixelStorei,    GLenum n, GLint p)
GL_STUB_VOID(glPolygonMode,    GLenum f, GLenum m)
GL_STUB_VOID(glPolygonOffset,  GLfloat f, GLfloat u)
GL_STUB_VOID(glReadBuffer,     GLenum m)
GL_STUB_VOID(glScissor,        GLint x, GLint y, GLsizei w, GLsizei h)
GL_STUB_VOID(glStencilFunc,    GLenum f, GLint r, GLuint m)
GL_STUB_VOID(glStencilMask,    GLuint m)
GL_STUB_VOID(glStencilOp,      GLenum f, GLenum zf, GLenum zp)
GL_STUB_VOID(glTexParameteri,  GLenum t, GLenum n, GLint p)
GL_STUB_VOID(glViewport,       GLint x, GLint y, GLsizei w, GLsizei h)

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{
   orbis_gl_once("glReadPixels");
}

void glTexSubImage2D(GLenum tg, GLint l, GLint x, GLint y, GLsizei w, GLsizei h,
                     GLenum f, GLenum t, const void *p)
{
   orbis_gl_once("glTexSubImage2D");
}

/* ⚠ THE FOUR THAT RETURN SOMETHING ARE THE ONLY DANGEROUS ONES. A caller that believes an
 * uninitialised out-parameter behaves worse than one told nothing, so each writes a defined
 * value; glGetString never returns NULL, because callers strstr() it for extension names. */
GLenum glGetError(void)
{
   orbis_gl_once("glGetError");
   return 0; /* GL_NO_ERROR - reporting a failure loop is worse than reporting none */
}

const GLubyte *glGetString(GLenum name)
{
   orbis_gl_once("glGetString");
   return (const GLubyte*)"";
}

void glGetIntegerv(GLenum pname, GLint *params)
{
   orbis_gl_once("glGetIntegerv");
   if (params)
      *params = 0;
}

void glGetFloatv(GLenum pname, GLfloat *params)
{
   orbis_gl_once("glGetFloatv");
   if (params)
      *params = 0.0f;
}
