#ifndef RME_GL_COMPAT_H_
#define RME_GL_COMPAT_H_

// OpenGL 1.2+ constants missing from Windows gl.h (OpenGL 1.1 only).
#ifndef GL_CLAMP_TO_EDGE
constexpr int GL_CLAMP_TO_EDGE = 0x812F;
#endif

#ifndef GL_BGRA
constexpr int GL_BGRA = 0x80E1;
#endif

#endif // RME_GL_COMPAT_H_
