// ndskit runtime — the OpenGL 4.5 core functions the renderer and the
// compositor use, loaded through SDL (display.cpp defines them, gl_load fills
// them in).
#pragma once
#include <GL/glcorearb.h>

#define NDSKIT_GL_FUNCS(X)                                                                          \
    X(PFNGLGETERRORPROC, glGetError)                                                                \
    X(PFNGLGETSTRINGPROC, glGetString)                                                              \
    X(PFNGLENABLEPROC, glEnable)                                                                    \
    X(PFNGLDISABLEPROC, glDisable)                                                                  \
    X(PFNGLVIEWPORTPROC, glViewport)                                                                \
    X(PFNGLCLEARPROC, glClear)                                                                      \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                                            \
    X(PFNGLCLEARDEPTHPROC, glClearDepth)                                                            \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                                              \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                                              \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                              \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)                                      \
    X(PFNGLBLENDFUNCSEPARATEIPROC, glBlendFuncSeparatei)                                            \
    X(PFNGLBLENDEQUATIONSEPARATEIPROC, glBlendEquationSeparatei)                                    \
    X(PFNGLENABLEIPROC, glEnablei)                                                                  \
    X(PFNGLDISABLEIPROC, glDisablei)                                                                \
    X(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC, glClearNamedFramebufferfv)                                  \
    X(PFNGLCLEARNAMEDFRAMEBUFFERIVPROC, glClearNamedFramebufferiv)                                  \
    X(PFNGLSTENCILFUNCPROC, glStencilFunc)                                                          \
    X(PFNGLSTENCILOPPROC, glStencilOp)                                                              \
    X(PFNGLCOLORMASKIPROC, glColorMaski)                                                            \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC, glNamedFramebufferDrawBuffers)                          \
    X(PFNGLPROGRAMUNIFORM4UIPROC, glProgramUniform4ui)                                              \
    X(PFNGLPOLYGONMODEPROC, glPolygonMode)                                                          \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                          \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                            \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                        \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                        \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                      \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                          \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                                \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                        \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                      \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                        \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                          \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                        \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                              \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                            \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                            \
    X(PFNGLPROGRAMUNIFORM1IPROC, glProgramUniform1i)                                                \
    X(PFNGLPROGRAMUNIFORM1UIPROC, glProgramUniform1ui)                                              \
    X(PFNGLPROGRAMUNIFORM1UIVPROC, glProgramUniform1uiv)                                            \
    X(PFNGLPROGRAMUNIFORM4FPROC, glProgramUniform4f)                                                \
    X(PFNGLCREATEBUFFERSPROC, glCreateBuffers)                                                      \
    X(PFNGLNAMEDBUFFERDATAPROC, glNamedBufferData)                                                  \
    X(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData)                                            \
    X(PFNGLCREATEVERTEXARRAYSPROC, glCreateVertexArrays)                                            \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                  \
    X(PFNGLVERTEXARRAYVERTEXBUFFERPROC, glVertexArrayVertexBuffer)                                  \
    X(PFNGLENABLEVERTEXARRAYATTRIBPROC, glEnableVertexArrayAttrib)                                  \
    X(PFNGLVERTEXARRAYATTRIBFORMATPROC, glVertexArrayAttribFormat)                                  \
    X(PFNGLVERTEXARRAYATTRIBIFORMATPROC, glVertexArrayAttribIFormat)                                \
    X(PFNGLVERTEXARRAYATTRIBBINDINGPROC, glVertexArrayAttribBinding)                                \
    X(PFNGLCREATETEXTURESPROC, glCreateTextures)                                                    \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D)                                                \
    X(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)                                              \
    X(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri)                                              \
    X(PFNGLTEXTUREBUFFERPROC, glTextureBuffer)                                                      \
    X(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit)                                                  \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                    \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers)                                            \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                            \
    X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture)                                  \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                  \
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus)                          \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                                            \
    X(PFNGLGETTEXTUREIMAGEPROC, glGetTextureImage)

#define NDSKIT_GL_DECLARE(T, n) extern T n;
NDSKIT_GL_FUNCS(NDSKIT_GL_DECLARE)
#undef NDSKIT_GL_DECLARE

GLuint gl_program(const char* vs, const char* fs);   // compiled and linked, or 0 (the log printed)

// render_gl.cpp: the rendering engine on the host's GPU
struct Render3dJob;
// the 3D's picture: colour and a fog flag per pixel drawn into fbo, the fog
// applied into fbo2's out
struct GlTarget { GLuint fbo = 0, color = 0, attr = 0, depth = 0, fbo2 = 0, out = 0; int scale = 0; };
bool render_init();
void render_target(GlTarget& t, int scale);           // (re)made at 256x192 times scale
GLuint render_draw(const Render3dJob& job, GlTarget& t); // the job's picture into t; its texture (rows bottom first)
