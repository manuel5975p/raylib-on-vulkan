/**********************************************************************************************
*
*   rlgl.h compatibility shim - raylib-on-vulkan
*
*   Upstream raylib code (and many examples) include "rlgl.h" for low-level
*   drawing. raylib-on-vulkan implements that API on Vulkan in rlvk.h with identical
*   names and semantics, so this shim just forwards.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#ifndef RLGL_H
#define RLGL_H

#include "rlvk.h"

// Version reported by upstream rlgl.h; kept for source compatibility
#define RLGL_VERSION "5.0-vk"

// Upstream rlgl.h exposes TRACELOG to its consumers; forward to raylib TraceLog
#ifndef TRACELOG
    #define TRACELOG(level, ...) TraceLog(level, __VA_ARGS__)
    #define TRACELOGD(...) TraceLog(LOG_DEBUG, __VA_ARGS__)
#endif

//----------------------------------------------------------------------------------
// Upstream rlgl.h compatibility constants (GL-style values, consumed by
// rlSetBlendFactors*/rlSetCullFace/rlBlitFramebuffer and friends)
//----------------------------------------------------------------------------------

// Blend factors
#define RL_ZERO                       0x0000
#define RL_ONE                        0x0001
#define RL_SRC_COLOR                  0x0300
#define RL_ONE_MINUS_SRC_COLOR        0x0301
#define RL_SRC_ALPHA                  0x0302
#define RL_ONE_MINUS_SRC_ALPHA        0x0303
#define RL_DST_ALPHA                  0x0304
#define RL_ONE_MINUS_DST_ALPHA        0x0305
#define RL_DST_COLOR                  0x0306
#define RL_ONE_MINUS_DST_COLOR        0x0307
#define RL_SRC_ALPHA_SATURATE         0x0308
#define RL_CONSTANT_COLOR             0x8001
#define RL_ONE_MINUS_CONSTANT_COLOR   0x8002
#define RL_CONSTANT_ALPHA             0x8003
#define RL_ONE_MINUS_CONSTANT_ALPHA   0x8004

// Blend equations
#define RL_FUNC_ADD                   0x8006
#define RL_MIN                        0x8007
#define RL_MAX                        0x8008
#define RL_FUNC_SUBTRACT              0x800A
#define RL_FUNC_REVERSE_SUBTRACT      0x800B
#define RL_BLEND_EQUATION             0x8009
#define RL_BLEND_EQUATION_RGB         0x8009
#define RL_BLEND_EQUATION_ALPHA       0x883D
#define RL_BLEND_DST_RGB              0x80C8
#define RL_BLEND_SRC_RGB              0x80C9
#define RL_BLEND_DST_ALPHA            0x80CA
#define RL_BLEND_SRC_ALPHA            0x80CB
#define RL_BLEND_COLOR                0x8005

// Cull face modes (rlSetCullFace)
#define RL_CULL_FACE_FRONT            0
#define RL_CULL_FACE_BACK             1

// Framebuffer targets (rlBindFramebuffer/rlBlitFramebuffer masks use GL values)
#define RL_READ_FRAMEBUFFER           0x8CA8
#define RL_DRAW_FRAMEBUFFER           0x8CA9
#define RL_COLOR_BUFFER_BIT           0x4000
#define RL_DEPTH_BUFFER_BIT           0x0100
#define RL_STENCIL_BUFFER_BIT         0x0400

// Attribute data types
#define RL_UNSIGNED_BYTE              0x1401
#define RL_FLOAT                      0x1406

// Pixel format aliases (identical values to raylib PixelFormat)
#define RL_PIXELFORMAT_UNCOMPRESSED_GRAYSCALE     1
#define RL_PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA    2
#define RL_PIXELFORMAT_UNCOMPRESSED_R5G6B5        3
#define RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8        4
#define RL_PIXELFORMAT_UNCOMPRESSED_R5G5B5A1      5
#define RL_PIXELFORMAT_UNCOMPRESSED_R4G4B4A4      6
#define RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8      7
#define RL_PIXELFORMAT_UNCOMPRESSED_R32           8
#define RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32     9
#define RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32A32  10
#define RL_PIXELFORMAT_UNCOMPRESSED_R16           11
#define RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16     12
#define RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16  13

// Shader uniform data type aliases (identical values to raylib ShaderUniformDataType)
#define RL_SHADER_UNIFORM_FLOAT       0
#define RL_SHADER_UNIFORM_VEC2        1
#define RL_SHADER_UNIFORM_VEC3        2
#define RL_SHADER_UNIFORM_VEC4        3
#define RL_SHADER_UNIFORM_INT         4
#define RL_SHADER_UNIFORM_IVEC2       5
#define RL_SHADER_UNIFORM_IVEC3       6
#define RL_SHADER_UNIFORM_IVEC4       7
#define RL_SHADER_UNIFORM_UINT        8
#define RL_SHADER_UNIFORM_UIVEC2      9
#define RL_SHADER_UNIFORM_UIVEC3      10
#define RL_SHADER_UNIFORM_UIVEC4      11
#define RL_SHADER_UNIFORM_SAMPLER2D   12

// Shader attribute data types
#define RL_SHADER_ATTRIB_FLOAT        0
#define RL_SHADER_ATTRIB_VEC2         1
#define RL_SHADER_ATTRIB_VEC3         2
#define RL_SHADER_ATTRIB_VEC4         3

// rlLoadDrawQuad: upstream helper drawing a unit quad (used by postprocess examples).
// Implemented over the immediate-mode batch; identical geometry/UVs to rlgl.
static inline void rlLoadDrawQuad(void)
{
    rlBegin(RL_QUADS);
        rlNormal3f(0.0f, 0.0f, 1.0f);
        rlTexCoord2f(0.0f, 0.0f); rlVertex3f(-1.0f, -1.0f, 0.0f);
        rlTexCoord2f(1.0f, 0.0f); rlVertex3f(1.0f, -1.0f, 0.0f);
        rlTexCoord2f(1.0f, 1.0f); rlVertex3f(1.0f, 1.0f, 0.0f);
        rlTexCoord2f(0.0f, 1.0f); rlVertex3f(-1.0f, 1.0f, 0.0f);
    rlEnd();
    rlDrawRenderBatchActive();
}

#endif // RLGL_H
