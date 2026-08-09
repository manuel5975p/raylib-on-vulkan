/**********************************************************************************************
*
*   rlvk - raylib OpenGL abstraction layer (rlgl) reimplemented on top of Vulkan 1.2+ (volk)
*
*   rlvk exposes the rlgl-compatible immediate-mode API used by the raylib modules
*   (rshapes, rtextures, rtext, rmodels) plus the device/swapchain lifecycle consumed
*   by rcore, and a small public interop API for custom Vulkan usage (see rayvk.h).
*
*   DESIGN
*     - Immediate-mode vertex emission accumulates into a CPU-side render batch;
*       the batch is flushed into the active frame command buffer as draw calls on
*       state changes (texture/mode/matrix/shader/blend) and at end of frame.
*     - One render pass per target (swapchain image or render texture), begun lazily
*       on first draw/clear, ended on target switch or frame end.
*     - Matrix stack semantics identical to rlgl (column-major, right-handed).
*     - Textures/framebuffers/shaders are opaque uint32 handles mapping to internal
*       tables of Vulkan objects (VkImage/VkImageView/VkSampler/VkFramebuffer/...).
*     - Shaders are SPIR-V modules. Uniform access for user shaders is resolved via
*       a minimal SPIR-V reflection pass (OpName/OpMemberName/OpDecorate parsing).
*
*   SHADER ABI (all pipelines, default and user):
*     vertex inputs:  location 0 vec3 vertexPosition
*                     location 1 vec2 vertexTexCoord
*                     location 2 vec3 vertexNormal
*                     location 3 vec4 vertexColor
*                     location 4 vec4 vertexTangent      (models only)
*                     location 5 vec2 vertexTexCoord2    (models only)
*                     location 6 uvec4 vertexBoneIds     (skinning)
*                     location 7 vec4 vertexBoneWeights  (skinning)
*     push constants: layout(push_constant) uniform PC { mat4 mvp; mat4 matModel; }
*     set 0 binding 0: uniform UBO { ... user uniforms, incl. vec4 colDiffuse ... }
*     set 1 binding 0..3: combined image samplers texture0..texture3
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#ifndef RLVK_H
#define RLVK_H

#include <stdbool.h>
#include <stdint.h>

#include "raylib.h"     // Matrix, Color, PixelFormat

//----------------------------------------------------------------------------------
// Defines (kept binary-compatible with rlgl.h so module code ports 1:1)
//----------------------------------------------------------------------------------
#define RL_DEFAULT_BATCH_BUFFER_ELEMENTS   8192    // vertices per batch buffer
#define RL_DEFAULT_BATCH_DRAWCALLS          256
#define RL_MAX_MATRIX_STACK_SIZE             32
#define RL_CULL_DISTANCE_NEAR             0.001    // Default projection matrix near cull distance
#define RL_CULL_DISTANCE_FAR             1000.0    // Default projection matrix far cull distance

// Primitive assembly draw modes
#define RL_LINES        0x0001
#define RL_TRIANGLES    0x0004
#define RL_QUADS        0x0007

// Matrix modes
#define RL_MODELVIEW    0x1700
#define RL_PROJECTION   0x1701

// Texture parameters
#define RL_TEXTURE_WRAP_S               0x2802
#define RL_TEXTURE_WRAP_T               0x2803
#define RL_TEXTURE_MAG_FILTER           0x2800
#define RL_TEXTURE_MIN_FILTER           0x2801

#define RL_TEXTURE_FILTER_NEAREST       0x2600
#define RL_TEXTURE_FILTER_LINEAR        0x2601
#define RL_TEXTURE_FILTER_MIP_NEAREST   0x2700
#define RL_TEXTURE_FILTER_NEAREST_MIP_LINEAR 0x2702
#define RL_TEXTURE_FILTER_LINEAR_MIP_NEAREST 0x2701
#define RL_TEXTURE_FILTER_MIP_LINEAR    0x2703
#define RL_TEXTURE_FILTER_ANISOTROPIC   0x3000

#define RL_TEXTURE_WRAP_REPEAT          0x2901
#define RL_TEXTURE_WRAP_CLAMP           0x812F
#define RL_TEXTURE_WRAP_MIRROR_REPEAT   0x8370
#define RL_TEXTURE_WRAP_MIRROR_CLAMP    0x8742

// Framebuffer attachment type/points (rlFramebufferAttach)
typedef enum {
    RL_ATTACHMENT_COLOR_CHANNEL0 = 0,
    RL_ATTACHMENT_COLOR_CHANNEL1 = 1,
    RL_ATTACHMENT_COLOR_CHANNEL2 = 2,
    RL_ATTACHMENT_COLOR_CHANNEL3 = 3,
    RL_ATTACHMENT_DEPTH = 100,
    RL_ATTACHMENT_STENCIL = 200
} rlFramebufferAttachType;

typedef enum {
    RL_ATTACHMENT_CUBEMAP_POSITIVE_X = 0,
    RL_ATTACHMENT_CUBEMAP_NEGATIVE_X = 1,
    RL_ATTACHMENT_CUBEMAP_POSITIVE_Y = 2,
    RL_ATTACHMENT_CUBEMAP_NEGATIVE_Y = 3,
    RL_ATTACHMENT_CUBEMAP_POSITIVE_Z = 4,
    RL_ATTACHMENT_CUBEMAP_NEGATIVE_Z = 5,
    RL_ATTACHMENT_TEXTURE2D = 100,
    RL_ATTACHMENT_RENDERBUFFER = 200
} rlFramebufferAttachTextureType;

// Default shader locations/uniform names follow raylib conventions:
//   "mvp", "matModel", "matView", "matProjection", "matNormal",
//   "colDiffuse", "texture0", "texture1", "texture2"

#if defined(__cplusplus)
extern "C" {
#endif

//----------------------------------------------------------------------------------
// Device / frame lifecycle (called by rcore only)
//----------------------------------------------------------------------------------

// Platform glue: rcore passes these so rlvk can create instance surface + swapchain.
// createSurface must call the platform's vkCreate*SurfaceKHR on the given VkInstance
// (passed as void* to keep this header vulkan-free) and return VkSurfaceKHR (as uint64).
// portability: set by backends running on a portability driver (MoltenVK on macOS).
// When true, rlvkInit MUST create the instance with
// VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR in VkInstanceCreateInfo::flags
// (the backend also lists VK_KHR_portability_enumeration in requiredExtensions) and
// enable VK_KHR_portability_subset on the device when the physical device reports it.
// Without this, vkEnumeratePhysicalDevices() returns zero devices on macOS.
typedef struct rlvkPlatformGlue {
    const char **requiredExtensions;   // instance extensions (VK_KHR_surface + platform)
    int requiredExtensionCount;
    uint64_t (*createSurface)(void *vkInstance, void *userData);
    void *userData;
    bool portability;                  // Portability driver (MoltenVK): see note above
} rlvkPlatformGlue;

// Initialize Vulkan: volk, instance, device, swapchain (width/height), default
// pipelines, default white texture, default shader, render batch. vsync toggles
// FIFO vs MAILBOX/IMMEDIATE present mode. Returns false on failure.
bool rlvkInit(const rlvkPlatformGlue *glue, int width, int height, bool vsync);
void rlvkClose(void);                          // Destroy all Vulkan objects (waits device idle)
bool rlvkIsReady(void);                        // Device and swapchain valid
void rlvkResize(int width, int height);        // Recreate swapchain, e.g. on window resize
void rlvkSetVSync(bool vsync);                 // Recreate swapchain with new present mode

void rlvkBeginFrame(void);                     // Acquire image, begin frame command buffer
void rlvkEndFrame(void);                       // Flush batch, end render pass, submit, present

//----------------------------------------------------------------------------------
// Matrix operations (rlgl-compatible)
//----------------------------------------------------------------------------------
void rlMatrixMode(int mode);                   // RL_PROJECTION or RL_MODELVIEW
void rlPushMatrix(void);
void rlPopMatrix(void);
void rlLoadIdentity(void);
void rlTranslatef(float x, float y, float z);
void rlRotatef(float angle, float x, float y, float z);   // angle in degrees
void rlScalef(float x, float y, float z);
void rlMultMatrixf(const float *matf);         // 16 floats, column-major
void rlFrustum(double left, double right, double bottom, double top, double znear, double zfar);
void rlOrtho(double left, double right, double bottom, double top, double znear, double zfar);
void rlViewport(int x, int y, int width, int height);
void rlSetClipPlanes(double nearPlane, double farPlane);
double rlGetCullDistanceNear(void);
double rlGetCullDistanceFar(void);

Matrix rlGetMatrixModelview(void);
Matrix rlGetMatrixProjection(void);
Matrix rlGetMatrixTransform(void);
void rlSetMatrixProjection(Matrix proj);
void rlSetMatrixModelview(Matrix view);

//----------------------------------------------------------------------------------
// Vertex emission (immediate mode, batched)
//----------------------------------------------------------------------------------
void rlBegin(int mode);                        // RL_LINES / RL_TRIANGLES / RL_QUADS
void rlEnd(void);
void rlVertex2i(int x, int y);
void rlVertex2f(float x, float y);
void rlVertex3f(float x, float y, float z);
void rlTexCoord2f(float x, float y);
void rlNormal3f(float x, float y, float z);
void rlColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void rlColor3f(float x, float y, float z);
void rlColor4f(float x, float y, float z, float w);

bool rlCheckRenderBatchLimit(int vCount);      // Flush batch if vCount does not fit; true if flushed
void rlDrawRenderBatchActive(void);            // Force flush current batch draws

void rlSetTexture(unsigned int id);            // Texture for following quads (0 = default white)
unsigned int rlGetTextureIdDefault(void);      // 1x1 white RGBA8
unsigned int rlGetShaderIdDefault(void);
int *rlGetShaderLocsDefault(void);

//----------------------------------------------------------------------------------
// Render state
//----------------------------------------------------------------------------------
void rlClearColor(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void rlClearScreenBuffers(void);               // Clear color+depth of current target

void rlEnableDepthTest(void);
void rlDisableDepthTest(void);
void rlEnableDepthMask(void);
void rlDisableDepthMask(void);
void rlEnableBackfaceCulling(void);
void rlDisableBackfaceCulling(void);
void rlSetCullFace(int mode);                  // 0 = front, 1 = back
void rlEnableWireMode(void);
void rlDisableWireMode(void);
void rlEnablePointMode(void);
void rlSetLineWidth(float width);
float rlGetLineWidth(void);
void rlEnableSmoothLines(void);
void rlDisableSmoothLines(void);

void rlEnableColorBlend(void);
void rlDisableColorBlend(void);
void rlSetBlendMode(int mode);                 // BlendMode from raylib.h
void rlSetBlendFactors(int srcFactor, int dstFactor, int equation);            // GL-style enums
void rlSetBlendFactorsSeparate(int srcRGB, int dstRGB, int srcAlpha, int dstAlpha, int eqRGB, int eqAlpha);

void rlEnableScissorTest(void);
void rlDisableScissorTest(void);
void rlScissor(int x, int y, int width, int height);   // Origin bottom-left (rlgl convention)

//----------------------------------------------------------------------------------
// Textures
//----------------------------------------------------------------------------------
// format is raylib PixelFormat. Compressed formats uploaded as-is when supported.
unsigned int rlLoadTexture(const void *data, int width, int height, int format, int mipmapCount);
unsigned int rlLoadTextureDepth(int width, int height, bool useRenderBuffer);
unsigned int rlLoadTextureCubemap(const void *data, int size, int format, int mipmapCount);
void rlUpdateTexture(unsigned int id, int offsetX, int offsetY, int width, int height, int format, const void *data);
void rlUnloadTexture(unsigned int id);
void rlGenTextureMipmaps(unsigned int id, int width, int height, int format, int *mipmaps);
void *rlReadTexturePixels(unsigned int id, int width, int height, int format);  // RL_MALLOC'd
unsigned char *rlReadScreenPixels(int width, int height);                       // RGBA, RL_MALLOC'd, top-left origin
void rlTextureParameters(unsigned int id, int param, int value);
// Select the sampler slot used by rlEnableTexture()/rlDisableTexture(). The shader ABI
// exposes exactly 4 combined image samplers (set 1, binding 0..3, texture0..texture3);
// selecting a slot outside 0..3 turns the following bind calls into no-ops (it does NOT
// alias slot 3) until a valid slot is selected again.
void rlActiveTextureSlot(int slot);                    // 0..3
void rlEnableTexture(unsigned int id);                 // Bind to active slot (batch texture path uses rlSetTexture)
void rlDisableTexture(void);
void rlEnableTextureCubemap(unsigned int id);
void rlDisableTextureCubemap(void);
void rlGetGlTextureFormats(int format, unsigned int *glInternalFormat, unsigned int *glFormat, unsigned int *glType); // Maps to VkFormat in glInternalFormat, others 0
const char *rlGetPixelFormatName(unsigned int format);

//----------------------------------------------------------------------------------
// Framebuffers (render textures)
//----------------------------------------------------------------------------------
unsigned int rlLoadFramebuffer(void);
void rlFramebufferAttach(unsigned int fboId, unsigned int texId, int attachType, int texType, int mipLevel);
bool rlFramebufferComplete(unsigned int id);
void rlUnloadFramebuffer(unsigned int id);
void rlEnableFramebuffer(unsigned int id);             // Switch render target (ends/begins render pass)
void rlDisableFramebuffer(void);                       // Back to swapchain target
void rlBindFramebuffer(unsigned int target, unsigned int framebuffer);
unsigned int rlGetActiveFramebuffer(void);
void rlBlitFramebuffer(int srcX, int srcY, int srcWidth, int srcHeight, int dstX, int dstY, int dstWidth, int dstHeight, int bufferMask);
// NOTE: clamps 'count' to the number of color attachments already attached to the active
// framebuffer, so rlFramebufferAttach() must be called BEFORE this (the reverse of the
// usual GL order, where glDrawBuffers() can be set up ahead of the attachments).
void rlActiveDrawBuffers(int count);

//----------------------------------------------------------------------------------
// Shaders (SPIR-V)
//----------------------------------------------------------------------------------
// vsCode/fsCode point to SPIR-V binary blobs (detected by 0x07230203 magic).
// GLSL source is NOT supported at runtime; compile offline with glslc/glslangValidator.
unsigned int rlLoadShaderCode(const char *vsCode, const char *fsCode);                 // NULL-terminated not required for SPIR-V; use rlLoadShaderCodeSize for exact sizes
unsigned int rlLoadShaderCodeSize(const void *vsSpirv, int vsSize, const void *fsSpirv, int fsSize);
void rlUnloadShaderProgram(unsigned int id);
int rlGetLocationUniform(unsigned int shaderId, const char *uniformName);              // Byte offset into set0 UBO (via reflection), -1 if not found
int rlGetLocationAttrib(unsigned int shaderId, const char *attribName);
void rlSetUniform(int locIndex, const void *value, int uniformType, int count);        // uniformType = ShaderUniformDataType
void rlSetUniformMatrix(int locIndex, Matrix mat);
void rlSetUniformMatrices(int locIndex, const Matrix *mat, int count);
void rlSetUniformSampler(int locIndex, unsigned int textureId);
void rlSetShader(unsigned int id, int *locs);          // Use shader for following batch draws
void rlEnableShader(unsigned int id);
void rlDisableShader(void);

//----------------------------------------------------------------------------------
// Mesh buffers (used by rmodels)
//----------------------------------------------------------------------------------
// bufferId slots follow raylib Mesh.vboId layout:
//   0=positions(vec3) 1=texcoords(vec2) 2=normals(vec3) 3=colors(u8vec4)
//   4=tangents(vec4) 5=texcoords2(vec2) 6=boneIds(u8vec4->uvec4) 7=boneWeights(vec4) 8=indices(u16)
#define RL_MAX_MESH_VERTEX_BUFFERS 9

unsigned int rlLoadVertexBuffer(const void *buffer, int size, bool dynamic);
unsigned int rlLoadVertexBufferElement(const void *buffer, int size, bool dynamic);
void rlUpdateVertexBuffer(unsigned int bufferId, const void *data, int dataSize, int offset);
void rlUpdateVertexBufferElements(unsigned int id, const void *data, int dataSize, int offset);
void rlUnloadVertexBuffer(unsigned int vboId);

// Draw an indexed/non-indexed mesh with currently set shader (rlSetShader/rlEnableShader),
// bound sampler uniforms and current modelview/projection. transforms may be NULL
// (identity) or point to 'instances' matrices (instances >= 1).
// vboIds: array of RL_MAX_MESH_VERTEX_BUFFERS handles, 0 where attribute absent;
// missing attributes are fed from a shared default buffer (white color, zero UV...).
void rlDrawMeshBuffers(const unsigned int *vboIds, int vertexCount, int indexCount,
                       const Matrix *transforms, int instances);

//----------------------------------------------------------------------------------
// Misc
//----------------------------------------------------------------------------------
int rlGetFramebufferWidth(void);
int rlGetFramebufferHeight(void);
void rlSetFramebufferWidth(int width);
void rlSetFramebufferHeight(int height);
const char *rlGetVersion(void);                        // "rlvk 1.0 (Vulkan)"

#if defined(__cplusplus)
}
#endif

#endif // RLVK_H
