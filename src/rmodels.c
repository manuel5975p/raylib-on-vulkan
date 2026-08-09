/**********************************************************************************************
*
*   rmodels - Basic 3d shapes drawing, 3d models loading/drawing, mesh generation,
*             materials, skeletal animation and 3d collision detection
*
*   IMPLEMENTATION NOTES (raylib-on-vulkan)
*     - All rendering goes through the rlvk (rlgl-compatible) layer, never OpenGL.
*     - Basic 3d shapes use the rlvk immediate-mode batch (rlBegin/rlVertex3f/rlEnd).
*     - Meshes live in rlvk vertex buffers (rlLoadVertexBuffer); Mesh.vaoId keeps the
*       raylib field but Vulkan has no VAOs, so it is used as a "uploaded" sentinel (1).
*     - DrawMesh()/DrawMeshInstanced() set the classic raylib uniforms
*       (mvp/matModel/matView/matProjection/matNormal/colDiffuse) whenever the material
*       shader exposes them (locs[...] != -1) and hand the model matrix to
*       rlDrawMeshBuffers(), which owns the push-constant mvp/matModel pair described in
*       the rlvk.h shader ABI. rlvk MUST therefore report -1 for uniforms that are backed
*       by push constants instead of the set-0 UBO, otherwise the transform is applied
*       twice. No contract extension was required beyond that documented behaviour.
*     - raylib 5.6-dev skeleton layout is used: bones/bind pose live in Model.skeleton,
*       the animated pose in Model.currentPose and the skinning palette in
*       Model.boneMatrices (Mesh has no per-mesh bone matrices in this API version).
*
*   Supported model file formats: OBJ, MTL, IQM, GLTF/GLB, VOX, M3D
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#include "raylib.h"

#include "config.h"
#include "rlvk.h"
#include "raymath.h"

#include <stdio.h>          // Required for: sprintf()
#include <stdlib.h>         // Required for: malloc(), calloc(), free()
#include <string.h>         // Required for: memcmp(), strlen(), strncpy()
#include <math.h>           // Required for: sinf(), cosf(), sqrtf(), fabsf(), fmodf()
#include <float.h>          // Required for: FLT_MAX
#include <stdint.h>         // Required for: uint16_t, uint32_t...

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
#ifndef RL_MALLOC
    #define RL_MALLOC(sz)           malloc(sz)
#endif
#ifndef RL_CALLOC
    #define RL_CALLOC(n,sz)         calloc(n,sz)
#endif
#ifndef RL_REALLOC
    #define RL_REALLOC(ptr,sz)      realloc(ptr,sz)
#endif
#ifndef RL_FREE
    #define RL_FREE(ptr)            free(ptr)
#endif

#ifndef MAX_MATERIAL_MAPS
    #define MAX_MATERIAL_MAPS       12      // Maximum number of maps supported
#endif
#ifndef MAX_MESH_VERTEX_BUFFERS
    #define MAX_MESH_VERTEX_BUFFERS RL_MAX_MESH_VERTEX_BUFFERS
#endif
#ifndef RL_MAX_SHADER_LOCATIONS
    #define RL_MAX_SHADER_LOCATIONS 32      // Maximum number of shader locations supported
#endif
#ifndef MAX_MESH_BONES
    #define MAX_MESH_BONES         256      // Maximum number of bones supported per mesh
#endif

// Mesh vertex buffer slot indices (mirror of the rlvk.h documented layout)
#define MESH_VBO_POSITION    0
#define MESH_VBO_TEXCOORD1   1
#define MESH_VBO_NORMAL      2
#define MESH_VBO_COLOR       3
#define MESH_VBO_TANGENT     4
#define MESH_VBO_TEXCOORD2   5
#define MESH_VBO_BONEIDS     6
#define MESH_VBO_BONEWEIGHTS 7
#define MESH_VBO_INDICES     8

//----------------------------------------------------------------------------------
// Third-party implementations owned by this module
//----------------------------------------------------------------------------------
#if defined(SUPPORT_FILEFORMAT_OBJ) || defined(SUPPORT_FILEFORMAT_MTL)
    #define TINYOBJ_MALLOC RL_MALLOC
    #define TINYOBJ_CALLOC RL_CALLOC
    #define TINYOBJ_REALLOC RL_REALLOC
    #define TINYOBJ_FREE RL_FREE

    #define TINYOBJ_LOADER_C_IMPLEMENTATION
    #include "external/tinyobj_loader_c.h"       // OBJ/MTL file format loading
#endif

#if defined(SUPPORT_FILEFORMAT_GLTF)
    #define CGLTF_MALLOC RL_MALLOC
    #define CGLTF_FREE RL_FREE

    #define CGLTF_IMPLEMENTATION
    #include "external/cgltf.h"                  // glTF file format loading
#endif

#if defined(SUPPORT_FILEFORMAT_VOX)
    #define VOX_MALLOC RL_MALLOC
    #define VOX_CALLOC RL_CALLOC
    #define VOX_REALLOC RL_REALLOC
    #define VOX_FREE RL_FREE

    #define VOX_LOADER_IMPLEMENTATION
    #include "external/vox_loader.h"             // MagicaVoxel file format loading
#endif

#if defined(SUPPORT_FILEFORMAT_M3D)
    #define M3D_MALLOC RL_MALLOC
    #define M3D_REALLOC RL_REALLOC
    #define M3D_FREE RL_FREE

    #define M3D_IMPLEMENTATION
    #include "external/m3d.h"                    // Model3D file format loading
#endif

#if defined(SUPPORT_MESH_GENERATION)
    #define PAR_MALLOC(T, N) ((T *)RL_MALLOC((N)*sizeof(T)))
    #define PAR_CALLOC(T, N) ((T *)RL_CALLOC((N)*sizeof(T), 1))
    #define PAR_REALLOC(T, BUF, N) ((T *)RL_REALLOC(BUF, (N)*sizeof(T)))
    #define PAR_FREE RL_FREE

    #if defined(_MSC_VER)
        #pragma warning(push)
        #pragma warning(disable : 4244)
    #elif defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wunused-parameter"
    #endif
    #define PAR_SHAPES_IMPLEMENTATION
    #include "external/par_shapes.h"             // Shapes 3d parametric generation
    #if defined(_MSC_VER)
        #pragma warning(pop)
    #elif defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic pop
    #endif
#endif

//----------------------------------------------------------------------------------
// Module internal functions declaration
//----------------------------------------------------------------------------------
static Model LoadOBJ(const char *fileName);
static Model LoadIQM(const char *fileName);
static ModelAnimation *LoadModelAnimationsIQM(const char *fileName, int *animCount);
static Model LoadGLTF(const char *fileName);
static ModelAnimation *LoadModelAnimationsGLTF(const char *fileName, int *animCount);
static Model LoadVOX(const char *fileName);
static Model LoadM3D(const char *fileName);
static ModelAnimation *LoadModelAnimationsM3D(const char *fileName, int *animCount);

static void BuildPoseFromParentJoints(const BoneInfo *bones, unsigned int boneCount, Transform *transforms);
static void SkeletonAllocDefaultPose(Model *model);

//----------------------------------------------------------------------------------
// Module internal helpers
//----------------------------------------------------------------------------------

// Emit a point of the unit sphere for the given ring/slice angles (degrees)
static void EmitSpherePoint(float ringDeg, float sliceDeg)
{
    const float ring = DEG2RAD*ringDeg;
    const float slice = DEG2RAD*sliceDeg;
    rlVertex3f(cosf(ring)*sinf(slice), sinf(ring), cosf(ring)*cosf(slice));
}

// Build the model matrix out of the classic position/axis/angle/scale parameters
static Matrix ComposeModelTransform(Vector3 position, Vector3 rotationAxis, float rotationAngle, Vector3 scale)
{
    const Matrix matScale = MatrixScale(scale.x, scale.y, scale.z);
    const Matrix matRotation = MatrixRotate(rotationAxis, rotationAngle*DEG2RAD);
    const Matrix matTranslation = MatrixTranslate(position.x, position.y, position.z);
    return MatrixMultiply(MatrixMultiply(matScale, matRotation), matTranslation);
}

// Check whether a material map slot holds a cubemap texture
static bool IsCubemapMaterialMap(int mapType)
{
    return ((mapType == MATERIAL_MAP_CUBEMAP) ||
            (mapType == MATERIAL_MAP_IRRADIANCE) ||
            (mapType == MATERIAL_MAP_PREFILTER));
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Basic 3d Shapes Drawing Functions
//----------------------------------------------------------------------------------

// Draw a line in 3D world space
void DrawLine3D(Vector3 startPos, Vector3 endPos, Color color)
{
    rlBegin(RL_LINES);
        rlColor4ub(color.r, color.g, color.b, color.a);
        rlVertex3f(startPos.x, startPos.y, startPos.z);
        rlVertex3f(endPos.x, endPos.y, endPos.z);
    rlEnd();
}

// Draw a point in 3D space, actually a small line
void DrawPoint3D(Vector3 position, Color color)
{
    rlPushMatrix();
        rlTranslatef(position.x, position.y, position.z);
        rlBegin(RL_LINES);
            rlColor4ub(color.r, color.g, color.b, color.a);
            rlVertex3f(0.0f, 0.0f, 0.0f);
            rlVertex3f(0.0f, 0.0f, 0.1f);
        rlEnd();
    rlPopMatrix();
}

// Draw a circle in 3D world space
void DrawCircle3D(Vector3 center, float radius, Vector3 rotationAxis, float rotationAngle, Color color)
{
    rlPushMatrix();
        rlTranslatef(center.x, center.y, center.z);
        rlRotatef(rotationAngle, rotationAxis.x, rotationAxis.y, rotationAxis.z);

        rlBegin(RL_LINES);
            for (int i = 0; i < 360; i += 10)
            {
                rlColor4ub(color.r, color.g, color.b, color.a);
                rlVertex3f(sinf(DEG2RAD*i)*radius, cosf(DEG2RAD*i)*radius, 0.0f);
                rlVertex3f(sinf(DEG2RAD*(i + 10))*radius, cosf(DEG2RAD*(i + 10))*radius, 0.0f);
            }
        rlEnd();
    rlPopMatrix();
}

// Draw a color-filled triangle (vertex in counter-clockwise order!)
void DrawTriangle3D(Vector3 v1, Vector3 v2, Vector3 v3, Color color)
{
    rlBegin(RL_TRIANGLES);
        rlColor4ub(color.r, color.g, color.b, color.a);
        rlVertex3f(v1.x, v1.y, v1.z);
        rlVertex3f(v2.x, v2.y, v2.z);
        rlVertex3f(v3.x, v3.y, v3.z);
    rlEnd();
}

// Draw a triangle strip defined by points
void DrawTriangleStrip3D(const Vector3 *points, int pointCount, Color color)
{
    if ((points == NULL) || (pointCount < 3)) return;

    rlBegin(RL_TRIANGLES);
        rlColor4ub(color.r, color.g, color.b, color.a);

        for (int i = 2; i < pointCount; i++)
        {
            if ((i%2) == 0)
            {
                rlVertex3f(points[i].x, points[i].y, points[i].z);
                rlVertex3f(points[i - 2].x, points[i - 2].y, points[i - 2].z);
                rlVertex3f(points[i - 1].x, points[i - 1].y, points[i - 1].z);
            }
            else
            {
                rlVertex3f(points[i].x, points[i].y, points[i].z);
                rlVertex3f(points[i - 1].x, points[i - 1].y, points[i - 1].z);
                rlVertex3f(points[i - 2].x, points[i - 2].y, points[i - 2].z);
            }
        }
    rlEnd();
}

// Draw cube
// NOTE: Cube position is the center position
void DrawCube(Vector3 position, float width, float height, float length, Color color)
{
    const float x = 0.0f;
    const float y = 0.0f;
    const float z = 0.0f;

    rlPushMatrix();
        rlTranslatef(position.x, position.y, position.z);

        rlBegin(RL_QUADS);
            rlColor4ub(color.r, color.g, color.b, color.a);

            // Front face
            rlNormal3f(0.0f, 0.0f, 1.0f);
            rlVertex3f(x - width/2, y - height/2, z + length/2);
            rlVertex3f(x + width/2, y - height/2, z + length/2);
            rlVertex3f(x + width/2, y + height/2, z + length/2);
            rlVertex3f(x - width/2, y + height/2, z + length/2);

            // Back face
            rlNormal3f(0.0f, 0.0f, -1.0f);
            rlVertex3f(x - width/2, y - height/2, z - length/2);
            rlVertex3f(x - width/2, y + height/2, z - length/2);
            rlVertex3f(x + width/2, y + height/2, z - length/2);
            rlVertex3f(x + width/2, y - height/2, z - length/2);

            // Top face
            rlNormal3f(0.0f, 1.0f, 0.0f);
            rlVertex3f(x - width/2, y + height/2, z - length/2);
            rlVertex3f(x - width/2, y + height/2, z + length/2);
            rlVertex3f(x + width/2, y + height/2, z + length/2);
            rlVertex3f(x + width/2, y + height/2, z - length/2);

            // Bottom face
            rlNormal3f(0.0f, -1.0f, 0.0f);
            rlVertex3f(x - width/2, y - height/2, z - length/2);
            rlVertex3f(x + width/2, y - height/2, z - length/2);
            rlVertex3f(x + width/2, y - height/2, z + length/2);
            rlVertex3f(x - width/2, y - height/2, z + length/2);

            // Right face
            rlNormal3f(1.0f, 0.0f, 0.0f);
            rlVertex3f(x + width/2, y - height/2, z - length/2);
            rlVertex3f(x + width/2, y + height/2, z - length/2);
            rlVertex3f(x + width/2, y + height/2, z + length/2);
            rlVertex3f(x + width/2, y - height/2, z + length/2);

            // Left face
            rlNormal3f(-1.0f, 0.0f, 0.0f);
            rlVertex3f(x - width/2, y - height/2, z - length/2);
            rlVertex3f(x - width/2, y - height/2, z + length/2);
            rlVertex3f(x - width/2, y + height/2, z + length/2);
            rlVertex3f(x - width/2, y + height/2, z - length/2);
        rlEnd();
    rlPopMatrix();
}

// Draw cube (Vector version)
void DrawCubeV(Vector3 position, Vector3 size, Color color)
{
    DrawCube(position, size.x, size.y, size.z, color);
}

// Draw cube wires
void DrawCubeWires(Vector3 position, float width, float height, float length, Color color)
{
    const float x = 0.0f;
    const float y = 0.0f;
    const float z = 0.0f;

    rlPushMatrix();
        rlTranslatef(position.x, position.y, position.z);

        rlBegin(RL_LINES);
            rlColor4ub(color.r, color.g, color.b, color.a);

            // Front face
            rlVertex3f(x - width/2, y - height/2, z + length/2);
            rlVertex3f(x + width/2, y - height/2, z + length/2);

            rlVertex3f(x + width/2, y - height/2, z + length/2);
            rlVertex3f(x + width/2, y + height/2, z + length/2);

            rlVertex3f(x + width/2, y + height/2, z + length/2);
            rlVertex3f(x - width/2, y + height/2, z + length/2);

            rlVertex3f(x - width/2, y + height/2, z + length/2);
            rlVertex3f(x - width/2, y - height/2, z + length/2);

            // Back face
            rlVertex3f(x - width/2, y - height/2, z - length/2);
            rlVertex3f(x + width/2, y - height/2, z - length/2);

            rlVertex3f(x + width/2, y - height/2, z - length/2);
            rlVertex3f(x + width/2, y + height/2, z - length/2);

            rlVertex3f(x + width/2, y + height/2, z - length/2);
            rlVertex3f(x - width/2, y + height/2, z - length/2);

            rlVertex3f(x - width/2, y + height/2, z - length/2);
            rlVertex3f(x - width/2, y - height/2, z - length/2);

            // Top face connectors
            rlVertex3f(x - width/2, y + height/2, z + length/2);
            rlVertex3f(x - width/2, y + height/2, z - length/2);

            rlVertex3f(x + width/2, y + height/2, z + length/2);
            rlVertex3f(x + width/2, y + height/2, z - length/2);

            // Bottom face connectors
            rlVertex3f(x - width/2, y - height/2, z + length/2);
            rlVertex3f(x - width/2, y - height/2, z - length/2);

            rlVertex3f(x + width/2, y - height/2, z + length/2);
            rlVertex3f(x + width/2, y - height/2, z - length/2);
        rlEnd();
    rlPopMatrix();
}

// Draw cube wires (Vector version)
void DrawCubeWiresV(Vector3 position, Vector3 size, Color color)
{
    DrawCubeWires(position, size.x, size.y, size.z, color);
}

// Draw sphere
void DrawSphere(Vector3 centerPos, float radius, Color color)
{
    DrawSphereEx(centerPos, radius, 16, 16, color);
}

// Draw sphere with extended parameters
void DrawSphereEx(Vector3 centerPos, float radius, int rings, int slices, Color color)
{
    if ((rings < 1) || (slices < 3)) return;

    rlPushMatrix();
        rlTranslatef(centerPos.x, centerPos.y, centerPos.z);
        rlScalef(radius, radius, radius);

        rlBegin(RL_TRIANGLES);
            rlColor4ub(color.r, color.g, color.b, color.a);

            const float ringStep = 180.0f/(float)(rings + 1);
            const float sliceStep = 360.0f/(float)slices;

            for (int i = 0; i < (rings + 2); i++)
            {
                const float r0 = 270.0f + ringStep*(float)i;
                const float r1 = 270.0f + ringStep*(float)(i + 1);

                for (int j = 0; j < slices; j++)
                {
                    const float s0 = sliceStep*(float)j;
                    const float s1 = sliceStep*(float)(j + 1);

                    EmitSpherePoint(r0, s0);
                    EmitSpherePoint(r1, s1);
                    EmitSpherePoint(r1, s0);

                    EmitSpherePoint(r0, s0);
                    EmitSpherePoint(r0, s1);
                    EmitSpherePoint(r1, s1);
                }
            }
        rlEnd();
    rlPopMatrix();
}

// Draw sphere wires
void DrawSphereWires(Vector3 centerPos, float radius, int rings, int slices, Color color)
{
    if ((rings < 1) || (slices < 3)) return;

    rlPushMatrix();
        rlTranslatef(centerPos.x, centerPos.y, centerPos.z);
        rlScalef(radius, radius, radius);

        rlBegin(RL_LINES);
            rlColor4ub(color.r, color.g, color.b, color.a);

            const float ringStep = 180.0f/(float)(rings + 1);
            const float sliceStep = 360.0f/(float)slices;

            for (int i = 0; i < (rings + 2); i++)
            {
                const float r0 = 270.0f + ringStep*(float)i;
                const float r1 = 270.0f + ringStep*(float)(i + 1);

                for (int j = 0; j < slices; j++)
                {
                    const float s0 = sliceStep*(float)j;
                    const float s1 = sliceStep*(float)(j + 1);

                    EmitSpherePoint(r0, s0);
                    EmitSpherePoint(r1, s1);

                    EmitSpherePoint(r1, s1);
                    EmitSpherePoint(r1, s0);

                    EmitSpherePoint(r1, s0);
                    EmitSpherePoint(r0, s0);
                }
            }
        rlEnd();
    rlPopMatrix();
}

// Draw a cylinder/cone
// NOTE: It could be also used for pyramid and cone
void DrawCylinder(Vector3 position, float radiusTop, float radiusBottom, float height, int slices, Color color)
{
    if (slices < 3) return;

    rlPushMatrix();
        rlTranslatef(position.x, position.y, position.z);

        rlBegin(RL_TRIANGLES);
            rlColor4ub(color.r, color.g, color.b, color.a);

            const float step = 360.0f/(float)slices;

            if (radiusTop > 0.0f)
            {
                // Draw the sides (quads split into two triangles)
                for (int i = 0; i < 360; i += (int)step)
                {
                    const float a0 = DEG2RAD*(float)i;
                    const float a1 = DEG2RAD*((float)i + step);

                    rlVertex3f(sinf(a0)*radiusBottom, 0.0f, cosf(a0)*radiusBottom);
                    rlVertex3f(sinf(a1)*radiusBottom, 0.0f, cosf(a1)*radiusBottom);
                    rlVertex3f(sinf(a1)*radiusTop, height, cosf(a1)*radiusTop);

                    rlVertex3f(sinf(a0)*radiusTop, height, cosf(a0)*radiusTop);
                    rlVertex3f(sinf(a0)*radiusBottom, 0.0f, cosf(a0)*radiusBottom);
                    rlVertex3f(sinf(a1)*radiusTop, height, cosf(a1)*radiusTop);
                }

                // Draw the top cap
                for (int i = 0; i < 360; i += (int)step)
                {
                    const float a0 = DEG2RAD*(float)i;
                    const float a1 = DEG2RAD*((float)i + step);

                    rlVertex3f(0.0f, height, 0.0f);
                    rlVertex3f(sinf(a0)*radiusTop, height, cosf(a0)*radiusTop);
                    rlVertex3f(sinf(a1)*radiusTop, height, cosf(a1)*radiusTop);
                }
            }
            else
            {
                // Draw the cone sides
                for (int i = 0; i < 360; i += (int)step)
                {
                    const float a0 = DEG2RAD*(float)i;
                    const float a1 = DEG2RAD*((float)i + step);

                    rlVertex3f(0.0f, height, 0.0f);
                    rlVertex3f(sinf(a0)*radiusBottom, 0.0f, cosf(a0)*radiusBottom);
                    rlVertex3f(sinf(a1)*radiusBottom, 0.0f, cosf(a1)*radiusBottom);
                }
            }

            // Draw the base cap
            for (int i = 0; i < 360; i += (int)step)
            {
                const float a0 = DEG2RAD*(float)i;
                const float a1 = DEG2RAD*((float)i + step);

                rlVertex3f(0.0f, 0.0f, 0.0f);
                rlVertex3f(sinf(a1)*radiusBottom, 0.0f, cosf(a1)*radiusBottom);
                rlVertex3f(sinf(a0)*radiusBottom, 0.0f, cosf(a0)*radiusBottom);
            }
        rlEnd();
    rlPopMatrix();
}

// Draw a cylinder with base at startPos and top at endPos
void DrawCylinderEx(Vector3 startPos, Vector3 endPos, float startRadius, float endRadius, int sides, Color color)
{
    if (sides < 3) return;

    const Vector3 direction = Vector3Subtract(endPos, startPos);
    if ((direction.x == 0.0f) && (direction.y == 0.0f) && (direction.z == 0.0f)) return;

    // Construct a basis of the base and the caps
    const Vector3 b1 = Vector3Normalize(Vector3Perpendicular(direction));
    const Vector3 b2 = Vector3Normalize(Vector3CrossProduct(b1, direction));

    const float baseAngle = 2.0f*PI/(float)sides;

    rlBegin(RL_TRIANGLES);
        rlColor4ub(color.r, color.g, color.b, color.a);

        for (int i = 0; i < sides; i++)
        {
            const float a0 = baseAngle*(float)i;
            const float a1 = baseAngle*(float)(i + 1);

            // NOTE: Compute the four corners of the side quad
            const Vector3 w0 = Vector3Add(Vector3Scale(b1, sinf(a0)), Vector3Scale(b2, cosf(a0)));
            const Vector3 w1 = Vector3Add(Vector3Scale(b1, sinf(a1)), Vector3Scale(b2, cosf(a1)));

            const Vector3 s0 = Vector3Add(startPos, Vector3Scale(w0, startRadius));
            const Vector3 s1 = Vector3Add(startPos, Vector3Scale(w1, startRadius));
            const Vector3 e0 = Vector3Add(endPos, Vector3Scale(w0, endRadius));
            const Vector3 e1 = Vector3Add(endPos, Vector3Scale(w1, endRadius));

            rlVertex3f(s1.x, s1.y, s1.z);
            rlVertex3f(s0.x, s0.y, s0.z);
            rlVertex3f(e0.x, e0.y, e0.z);

            rlVertex3f(s1.x, s1.y, s1.z);
            rlVertex3f(e0.x, e0.y, e0.z);
            rlVertex3f(e1.x, e1.y, e1.z);

            // Caps
            if (startRadius > 0.0f)
            {
                rlVertex3f(startPos.x, startPos.y, startPos.z);
                rlVertex3f(s0.x, s0.y, s0.z);
                rlVertex3f(s1.x, s1.y, s1.z);
            }

            if (endRadius > 0.0f)
            {
                rlVertex3f(endPos.x, endPos.y, endPos.z);
                rlVertex3f(e1.x, e1.y, e1.z);
                rlVertex3f(e0.x, e0.y, e0.z);
            }
        }
    rlEnd();
}

// Draw a cylinder/cone wires
void DrawCylinderWires(Vector3 position, float radiusTop, float radiusBottom, float height, int slices, Color color)
{
    if (slices < 3) return;

    rlPushMatrix();
        rlTranslatef(position.x, position.y, position.z);

        rlBegin(RL_LINES);
            rlColor4ub(color.r, color.g, color.b, color.a);

            const float step = 360.0f/(float)slices;

            for (int i = 0; i < 360; i += (int)step)
            {
                const float a0 = DEG2RAD*(float)i;
                const float a1 = DEG2RAD*((float)i + step);

                rlVertex3f(sinf(a0)*radiusBottom, 0.0f, cosf(a0)*radiusBottom);
                rlVertex3f(sinf(a1)*radiusBottom, 0.0f, cosf(a1)*radiusBottom);

                rlVertex3f(sinf(a1)*radiusBottom, 0.0f, cosf(a1)*radiusBottom);
                rlVertex3f(sinf(a1)*radiusTop, height, cosf(a1)*radiusTop);

                rlVertex3f(sinf(a1)*radiusTop, height, cosf(a1)*radiusTop);
                rlVertex3f(sinf(a0)*radiusTop, height, cosf(a0)*radiusTop);

                rlVertex3f(sinf(a0)*radiusTop, height, cosf(a0)*radiusTop);
                rlVertex3f(sinf(a0)*radiusBottom, 0.0f, cosf(a0)*radiusBottom);
            }
        rlEnd();
    rlPopMatrix();
}

// Draw a cylinder wires with base at startPos and top at endPos
void DrawCylinderWiresEx(Vector3 startPos, Vector3 endPos, float startRadius, float endRadius, int sides, Color color)
{
    if (sides < 3) return;

    const Vector3 direction = Vector3Subtract(endPos, startPos);
    if ((direction.x == 0.0f) && (direction.y == 0.0f) && (direction.z == 0.0f)) return;

    const Vector3 b1 = Vector3Normalize(Vector3Perpendicular(direction));
    const Vector3 b2 = Vector3Normalize(Vector3CrossProduct(b1, direction));

    const float baseAngle = 2.0f*PI/(float)sides;

    rlBegin(RL_LINES);
        rlColor4ub(color.r, color.g, color.b, color.a);

        for (int i = 0; i < sides; i++)
        {
            const float a0 = baseAngle*(float)i;
            const float a1 = baseAngle*(float)(i + 1);

            const Vector3 w0 = Vector3Add(Vector3Scale(b1, sinf(a0)), Vector3Scale(b2, cosf(a0)));
            const Vector3 w1 = Vector3Add(Vector3Scale(b1, sinf(a1)), Vector3Scale(b2, cosf(a1)));

            const Vector3 s0 = Vector3Add(startPos, Vector3Scale(w0, startRadius));
            const Vector3 s1 = Vector3Add(startPos, Vector3Scale(w1, startRadius));
            const Vector3 e0 = Vector3Add(endPos, Vector3Scale(w0, endRadius));
            const Vector3 e1 = Vector3Add(endPos, Vector3Scale(w1, endRadius));

            rlVertex3f(s0.x, s0.y, s0.z);
            rlVertex3f(s1.x, s1.y, s1.z);

            rlVertex3f(s0.x, s0.y, s0.z);
            rlVertex3f(e0.x, e0.y, e0.z);

            rlVertex3f(e0.x, e0.y, e0.z);
            rlVertex3f(e1.x, e1.y, e1.z);
        }
    rlEnd();
}

// Draw a capsule with the center of its sphere caps at startPos and endPos
// NOTE: raylib.h names the integer parameters (rings, slices); the positional meaning
// implemented here matches raylib upstream: first int = slices, second int = rings
void DrawCapsule(Vector3 startPos, Vector3 endPos, float radius, int slices, int rings, Color color)
{
    if ((slices < 3) || (rings < 3)) return;

    Vector3 direction = Vector3Subtract(endPos, startPos);

    // Draw a sphere if start and end points are the same
    const float length = Vector3Length(direction);
    if (length < EPSILON)
    {
        DrawSphereEx(startPos, radius, rings, slices, color);
        return;
    }

    direction = Vector3Scale(direction, 1.0f/length);

    // Construct a basis for the sphere caps
    const Vector3 b0 = direction;
    const Vector3 b1 = Vector3Normalize(Vector3Perpendicular(direction));
    const Vector3 b2 = Vector3Normalize(Vector3CrossProduct(b1, direction));
    const Vector3 capCenter[2] = { endPos, startPos };

    const float baseSliceAngle = 2.0f*PI/(float)slices;
    const float baseRingAngle = PI*0.5f/(float)rings;

    rlBegin(RL_TRIANGLES);
        rlColor4ub(color.r, color.g, color.b, color.a);

        // Render both caps
        for (int c = 0; c < 2; c++)
        {
            const Vector3 center = capCenter[c];
            const float sign = (c == 0)? 1.0f : -1.0f;

            for (int i = 0; i < rings; i++)
            {
                for (int j = 0; j < slices; j++)
                {
                    // We build a quad on the sphere surface (two triangles)
                    const float ring0 = baseRingAngle*(float)i;
                    const float ring1 = baseRingAngle*(float)(i + 1);
                    const float slice0 = baseSliceAngle*(float)j;
                    const float slice1 = baseSliceAngle*(float)(j + 1);

                    const float lat[2] = { ring0, ring1 };
                    const float lon[2] = { slice0, slice1 };
                    Vector3 p[4];

                    for (int k = 0; k < 4; k++)
                    {
                        const float la = lat[k >> 1];
                        const float lo = lon[(k == 1) || (k == 2)];
                        const Vector3 dir = Vector3Add(Vector3Add(
                            Vector3Scale(b0, sign*sinf(la)),
                            Vector3Scale(b1, cosf(la)*sinf(lo))),
                            Vector3Scale(b2, cosf(la)*cosf(lo)));
                        p[k] = Vector3Add(center, Vector3Scale(dir, radius));
                    }

                    // p0 = (lat0, lon0), p1 = (lat0, lon1), p2 = (lat1, lon1), p3 = (lat1, lon0)
                    if (c == 0)
                    {
                        rlVertex3f(p[0].x, p[0].y, p[0].z);
                        rlVertex3f(p[1].x, p[1].y, p[1].z);
                        rlVertex3f(p[2].x, p[2].y, p[2].z);

                        rlVertex3f(p[0].x, p[0].y, p[0].z);
                        rlVertex3f(p[2].x, p[2].y, p[2].z);
                        rlVertex3f(p[3].x, p[3].y, p[3].z);
                    }
                    else
                    {
                        rlVertex3f(p[0].x, p[0].y, p[0].z);
                        rlVertex3f(p[2].x, p[2].y, p[2].z);
                        rlVertex3f(p[1].x, p[1].y, p[1].z);

                        rlVertex3f(p[0].x, p[0].y, p[0].z);
                        rlVertex3f(p[3].x, p[3].y, p[3].z);
                        rlVertex3f(p[2].x, p[2].y, p[2].z);
                    }
                }
            }
        }

        // Render the middle cylinder
        for (int j = 0; j < slices; j++)
        {
            const float lon0 = baseSliceAngle*(float)j;
            const float lon1 = baseSliceAngle*(float)(j + 1);

            const Vector3 w0 = Vector3Add(Vector3Scale(b1, sinf(lon0)), Vector3Scale(b2, cosf(lon0)));
            const Vector3 w1 = Vector3Add(Vector3Scale(b1, sinf(lon1)), Vector3Scale(b2, cosf(lon1)));

            const Vector3 s0 = Vector3Add(startPos, Vector3Scale(w0, radius));
            const Vector3 s1 = Vector3Add(startPos, Vector3Scale(w1, radius));
            const Vector3 e0 = Vector3Add(endPos, Vector3Scale(w0, radius));
            const Vector3 e1 = Vector3Add(endPos, Vector3Scale(w1, radius));

            rlVertex3f(s1.x, s1.y, s1.z);
            rlVertex3f(s0.x, s0.y, s0.z);
            rlVertex3f(e0.x, e0.y, e0.z);

            rlVertex3f(s1.x, s1.y, s1.z);
            rlVertex3f(e0.x, e0.y, e0.z);
            rlVertex3f(e1.x, e1.y, e1.z);
        }
    rlEnd();
}

// Draw capsule wireframe with the center of its sphere caps at startPos and endPos
void DrawCapsuleWires(Vector3 startPos, Vector3 endPos, float radius, int slices, int rings, Color color)
{
    if ((slices < 3) || (rings < 3)) return;

    Vector3 direction = Vector3Subtract(endPos, startPos);

    const float length = Vector3Length(direction);
    if (length < EPSILON)
    {
        DrawSphereWires(startPos, radius, rings, slices, color);
        return;
    }

    direction = Vector3Scale(direction, 1.0f/length);

    const Vector3 b0 = direction;
    const Vector3 b1 = Vector3Normalize(Vector3Perpendicular(direction));
    const Vector3 b2 = Vector3Normalize(Vector3CrossProduct(b1, direction));
    const Vector3 capCenter[2] = { endPos, startPos };

    const float baseSliceAngle = 2.0f*PI/(float)slices;
    const float baseRingAngle = PI*0.5f/(float)rings;

    rlBegin(RL_LINES);
        rlColor4ub(color.r, color.g, color.b, color.a);

        for (int c = 0; c < 2; c++)
        {
            const Vector3 center = capCenter[c];
            const float sign = (c == 0)? 1.0f : -1.0f;

            for (int i = 0; i < rings; i++)
            {
                for (int j = 0; j < slices; j++)
                {
                    const float lat[2] = { baseRingAngle*(float)i, baseRingAngle*(float)(i + 1) };
                    const float lon[2] = { baseSliceAngle*(float)j, baseSliceAngle*(float)(j + 1) };
                    Vector3 p[4];

                    for (int k = 0; k < 4; k++)
                    {
                        const float la = lat[k >> 1];
                        const float lo = lon[(k == 1) || (k == 2)];
                        const Vector3 dir = Vector3Add(Vector3Add(
                            Vector3Scale(b0, sign*sinf(la)),
                            Vector3Scale(b1, cosf(la)*sinf(lo))),
                            Vector3Scale(b2, cosf(la)*cosf(lo)));
                        p[k] = Vector3Add(center, Vector3Scale(dir, radius));
                    }

                    rlVertex3f(p[0].x, p[0].y, p[0].z);
                    rlVertex3f(p[1].x, p[1].y, p[1].z);

                    rlVertex3f(p[1].x, p[1].y, p[1].z);
                    rlVertex3f(p[2].x, p[2].y, p[2].z);

                    rlVertex3f(p[2].x, p[2].y, p[2].z);
                    rlVertex3f(p[3].x, p[3].y, p[3].z);
                }
            }
        }

        for (int j = 0; j < slices; j++)
        {
            const float lon = baseSliceAngle*(float)j;
            const Vector3 w = Vector3Add(Vector3Scale(b1, sinf(lon)), Vector3Scale(b2, cosf(lon)));
            const Vector3 s = Vector3Add(startPos, Vector3Scale(w, radius));
            const Vector3 e = Vector3Add(endPos, Vector3Scale(w, radius));

            rlVertex3f(s.x, s.y, s.z);
            rlVertex3f(e.x, e.y, e.z);
        }
    rlEnd();
}

// Draw a plane
void DrawPlane(Vector3 centerPos, Vector2 size, Color color)
{
    // NOTE: Plane is always created on XZ ground and then rotated
    rlPushMatrix();
        rlTranslatef(centerPos.x, centerPos.y, centerPos.z);
        rlScalef(size.x, 1.0f, size.y);

        rlBegin(RL_QUADS);
            rlColor4ub(color.r, color.g, color.b, color.a);
            rlNormal3f(0.0f, 1.0f, 0.0f);

            rlVertex3f(-0.5f, 0.0f, -0.5f);
            rlVertex3f(-0.5f, 0.0f, 0.5f);
            rlVertex3f(0.5f, 0.0f, 0.5f);
            rlVertex3f(0.5f, 0.0f, -0.5f);
        rlEnd();
    rlPopMatrix();
}

// Draw a ray line
void DrawRay(Ray ray, Color color)
{
    const float scale = 10000.0f;

    rlBegin(RL_LINES);
        rlColor4ub(color.r, color.g, color.b, color.a);
        rlColor4ub(color.r, color.g, color.b, color.a);

        rlVertex3f(ray.position.x, ray.position.y, ray.position.z);
        rlVertex3f(ray.position.x + ray.direction.x*scale,
                   ray.position.y + ray.direction.y*scale,
                   ray.position.z + ray.direction.z*scale);
    rlEnd();
}

// Draw a grid centered at (0, 0, 0)
void DrawGrid(int slices, float spacing)
{
    const int halfSlices = slices/2;

    rlBegin(RL_LINES);
        for (int i = -halfSlices; i <= halfSlices; i++)
        {
            if (i == 0) rlColor3f(0.5f, 0.5f, 0.5f);
            else rlColor3f(0.75f, 0.75f, 0.75f);

            rlVertex3f((float)i*spacing, 0.0f, (float)-halfSlices*spacing);
            rlVertex3f((float)i*spacing, 0.0f, (float)halfSlices*spacing);

            rlVertex3f((float)-halfSlices*spacing, 0.0f, (float)i*spacing);
            rlVertex3f((float)halfSlices*spacing, 0.0f, (float)i*spacing);
        }
    rlEnd();
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Model 3d Loading and Drawing Functions
//----------------------------------------------------------------------------------

// Load model from files (mesh and material)
Model LoadModel(const char *fileName)
{
    Model model = { 0 };

    if (fileName == NULL)
    {
        TRACELOG(LOG_WARNING, "MODEL: File name provided is not valid");
        return model;
    }

#if defined(SUPPORT_FILEFORMAT_OBJ)
    if (IsFileExtension(fileName, ".obj")) model = LoadOBJ(fileName);
#endif
#if defined(SUPPORT_FILEFORMAT_IQM)
    if (IsFileExtension(fileName, ".iqm")) model = LoadIQM(fileName);
#endif
#if defined(SUPPORT_FILEFORMAT_GLTF)
    if (IsFileExtension(fileName, ".gltf") || IsFileExtension(fileName, ".glb")) model = LoadGLTF(fileName);
#endif
#if defined(SUPPORT_FILEFORMAT_VOX)
    if (IsFileExtension(fileName, ".vox")) model = LoadVOX(fileName);
#endif
#if defined(SUPPORT_FILEFORMAT_M3D)
    if (IsFileExtension(fileName, ".m3d")) model = LoadM3D(fileName);
#endif

    // Make sure model transform is set to identity matrix!
    model.transform = MatrixIdentity();

    if ((model.meshCount != 0) && (model.meshes != NULL))
    {
        // Upload vertex data to GPU (static meshes)
        for (int i = 0; i < model.meshCount; i++)
        {
            if (model.meshes[i].vaoId == 0) UploadMesh(&model.meshes[i], false);
        }
    }
    else TRACELOG(LOG_WARNING, "MESH: [%s] Failed to load model mesh(es) data", fileName);

    if (model.materialCount == 0)
    {
        TRACELOG(LOG_WARNING, "MATERIAL: [%s] Failed to load model material data, default to white material", fileName);

        model.materialCount = 1;
        model.materials = (Material *)RL_CALLOC(model.materialCount, sizeof(Material));
        if (model.materials != NULL) model.materials[0] = LoadMaterialDefault();

        if (model.meshMaterial == NULL) model.meshMaterial = (int *)RL_CALLOC(model.meshCount, sizeof(int));
    }

    SkeletonAllocDefaultPose(&model);

    return model;
}

// Load model from generated mesh
// WARNING: A shallow copy of mesh is generated, passed by value,
// as long as struct contains pointers to data and some values, we get a copy
// of mesh pointing to same data as original version... be careful!
Model LoadModelFromMesh(Mesh mesh)
{
    Model model = { 0 };

    model.transform = MatrixIdentity();

    model.meshCount = 1;
    model.meshes = (Mesh *)RL_CALLOC(model.meshCount, sizeof(Mesh));
    if (model.meshes == NULL)
    {
        model.meshCount = 0;
        return model;
    }
    model.meshes[0] = mesh;

    model.materialCount = 1;
    model.materials = (Material *)RL_CALLOC(model.materialCount, sizeof(Material));
    if (model.materials != NULL) model.materials[0] = LoadMaterialDefault();

    model.meshMaterial = (int *)RL_CALLOC(model.meshCount, sizeof(int));

    return model;
}

// Check if a model is valid (loaded in GPU, VAO/VBOs)
bool IsModelValid(Model model)
{
    if ((model.meshes == NULL) ||
        (model.materials == NULL) ||
        (model.meshMaterial == NULL) ||
        (model.meshCount <= 0) ||
        (model.materialCount <= 0)) return false;

    // Check meshes and materials
    for (int i = 0; i < model.meshCount; i++)
    {
        if ((model.meshes[i].vertices == NULL) && (model.meshes[i].vboId == NULL)) return false;
        if (model.meshes[i].vboId == NULL) return false;
        if (model.meshes[i].vboId[MESH_VBO_POSITION] == 0) return false;

        if ((model.meshMaterial[i] < 0) || (model.meshMaterial[i] >= model.materialCount)) return false;
    }

    for (int i = 0; i < model.materialCount; i++)
    {
        if (model.materials[i].maps == NULL) return false;
    }

    return true;
}

// Unload model (meshes/materials) from memory (RAM and/or VRAM)
// NOTE: This function takes care of all model elements, for a detailed control
// over them, use UnloadMesh() and UnloadMaterial()
void UnloadModel(Model model)
{
    // Unload meshes
    for (int i = 0; i < model.meshCount; i++) UnloadMesh(model.meshes[i]);

    // Unload materials maps
    // NOTE: As the user could be sharing shaders and textures between models,
    // we don't unload the material but just free its maps,
    // the user is responsible for freeing models shaders and textures
    for (int i = 0; i < model.materialCount; i++) RL_FREE(model.materials[i].maps);

    RL_FREE(model.meshes);
    RL_FREE(model.materials);
    RL_FREE(model.meshMaterial);

    // Unload animation data
    RL_FREE(model.skeleton.bones);
    RL_FREE(model.skeleton.bindPose);
    RL_FREE(model.currentPose);
    RL_FREE(model.boneMatrices);

    TRACELOG(LOG_INFO, "MODEL: Unloaded model (and meshes) from RAM and VRAM");
}

// Compute model bounding box limits (considers all meshes)
BoundingBox GetModelBoundingBox(Model model)
{
    BoundingBox bounds = { 0 };

    if ((model.meshCount == 0) || (model.meshes == NULL)) return bounds;

    bounds = GetMeshBoundingBox(model.meshes[0]);

    for (int i = 1; i < model.meshCount; i++)
    {
        const BoundingBox tempBounds = GetMeshBoundingBox(model.meshes[i]);

        bounds.min = Vector3Min(bounds.min, tempBounds.min);
        bounds.max = Vector3Max(bounds.max, tempBounds.max);
    }

    // Apply model.transform to bounding box
    // NOTE: Rotate the 8 corners and rebuild an axis-aligned box
    const Vector3 corners[8] = {
        { bounds.min.x, bounds.min.y, bounds.min.z },
        { bounds.min.x, bounds.min.y, bounds.max.z },
        { bounds.min.x, bounds.max.y, bounds.min.z },
        { bounds.min.x, bounds.max.y, bounds.max.z },
        { bounds.max.x, bounds.min.y, bounds.min.z },
        { bounds.max.x, bounds.min.y, bounds.max.z },
        { bounds.max.x, bounds.max.y, bounds.min.z },
        { bounds.max.x, bounds.max.y, bounds.max.z }
    };

    Vector3 tMin = Vector3Transform(corners[0], model.transform);
    Vector3 tMax = tMin;

    for (int i = 1; i < 8; i++)
    {
        const Vector3 v = Vector3Transform(corners[i], model.transform);
        tMin = Vector3Min(tMin, v);
        tMax = Vector3Max(tMax, v);
    }

    bounds.min = tMin;
    bounds.max = tMax;

    return bounds;
}

// Upload vertex data into a VAO (if supported) and VBO
void UploadMesh(Mesh *mesh, bool dynamic)
{
    if (mesh == NULL) return;

    if ((mesh->vboId != NULL) && (mesh->vaoId > 0))
    {
        // Check if mesh has already been loaded in GPU
        TRACELOG(LOG_WARNING, "VAO: [ID %i] Trying to re-load an already loaded mesh", mesh->vaoId);
        return;
    }

    mesh->vboId = (unsigned int *)RL_CALLOC(MAX_MESH_VERTEX_BUFFERS, sizeof(unsigned int));
    if (mesh->vboId == NULL)
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to allocate vertex buffer id array");
        return;
    }

    // NOTE: Vulkan has no vertex array objects, vaoId is kept as an "uploaded" sentinel
    mesh->vaoId = 1;

    if ((mesh->vertices != NULL) && (mesh->vertexCount > 0))
    {
        mesh->vboId[MESH_VBO_POSITION] = rlLoadVertexBuffer(mesh->vertices, mesh->vertexCount*3*(int)sizeof(float), dynamic);
    }

    if (mesh->texcoords != NULL)
    {
        mesh->vboId[MESH_VBO_TEXCOORD1] = rlLoadVertexBuffer(mesh->texcoords, mesh->vertexCount*2*(int)sizeof(float), dynamic);
    }

    if (mesh->normals != NULL)
    {
        mesh->vboId[MESH_VBO_NORMAL] = rlLoadVertexBuffer(mesh->normals, mesh->vertexCount*3*(int)sizeof(float), dynamic);
    }

    if (mesh->colors != NULL)
    {
        mesh->vboId[MESH_VBO_COLOR] = rlLoadVertexBuffer(mesh->colors, mesh->vertexCount*4*(int)sizeof(unsigned char), dynamic);
    }

    if (mesh->tangents != NULL)
    {
        mesh->vboId[MESH_VBO_TANGENT] = rlLoadVertexBuffer(mesh->tangents, mesh->vertexCount*4*(int)sizeof(float), dynamic);
    }

    if (mesh->texcoords2 != NULL)
    {
        mesh->vboId[MESH_VBO_TEXCOORD2] = rlLoadVertexBuffer(mesh->texcoords2, mesh->vertexCount*2*(int)sizeof(float), dynamic);
    }

    if (mesh->boneIndices != NULL)
    {
        mesh->vboId[MESH_VBO_BONEIDS] = rlLoadVertexBuffer(mesh->boneIndices, mesh->vertexCount*4*(int)sizeof(unsigned char), dynamic);
    }

    if (mesh->boneWeights != NULL)
    {
        mesh->vboId[MESH_VBO_BONEWEIGHTS] = rlLoadVertexBuffer(mesh->boneWeights, mesh->vertexCount*4*(int)sizeof(float), dynamic);
    }

    if (mesh->indices != NULL)
    {
        mesh->vboId[MESH_VBO_INDICES] = rlLoadVertexBufferElement(mesh->indices, mesh->triangleCount*3*(int)sizeof(unsigned short), dynamic);
    }

    TRACELOG(LOG_INFO, "MESH: Mesh uploaded to GPU (%i vertices, %i triangles)", mesh->vertexCount, mesh->triangleCount);
}

// Update mesh vertex data in GPU for a specific buffer index
void UpdateMeshBuffer(Mesh mesh, int index, const void *data, int dataSize, int offset)
{
    if ((mesh.vboId == NULL) || (index < 0) || (index >= MAX_MESH_VERTEX_BUFFERS))
    {
        TRACELOG(LOG_WARNING, "MESH: Trying to update an invalid vertex buffer index (%i)", index);
        return;
    }

    if (mesh.vboId[index] == 0) return;

    if (index == MESH_VBO_INDICES) rlUpdateVertexBufferElements(mesh.vboId[index], data, dataSize, offset);
    else rlUpdateVertexBuffer(mesh.vboId[index], data, dataSize, offset);
}

// Unload mesh from memory (RAM and VRAM)
void UnloadMesh(Mesh mesh)
{
    // Unload rlvk mesh vertex buffers
    if (mesh.vboId != NULL)
    {
        for (int i = 0; i < MAX_MESH_VERTEX_BUFFERS; i++) rlUnloadVertexBuffer(mesh.vboId[i]);
        RL_FREE(mesh.vboId);
    }

    RL_FREE(mesh.vertices);
    RL_FREE(mesh.texcoords);
    RL_FREE(mesh.normals);
    RL_FREE(mesh.colors);
    RL_FREE(mesh.tangents);
    RL_FREE(mesh.texcoords2);
    RL_FREE(mesh.indices);

    RL_FREE(mesh.animVertices);
    RL_FREE(mesh.animNormals);
    RL_FREE(mesh.boneWeights);
    RL_FREE(mesh.boneIndices);
}

// Bind material maps textures to the currently enabled shader
static void BindMaterialTextures(Material material)
{
    for (int i = 0; i < MAX_MATERIAL_MAPS; i++)
    {
        if (material.maps[i].texture.id == 0) continue;

        rlActiveTextureSlot(i);

        if (IsCubemapMaterialMap(i)) rlEnableTextureCubemap(material.maps[i].texture.id);
        else rlEnableTexture(material.maps[i].texture.id);

        if (material.shader.locs != NULL)
        {
            rlSetUniformSampler(material.shader.locs[SHADER_LOC_MAP_DIFFUSE + i], material.maps[i].texture.id);
        }
    }
}

// Unbind material maps textures
static void UnbindMaterialTextures(Material material)
{
    for (int i = 0; i < MAX_MATERIAL_MAPS; i++)
    {
        if (material.maps[i].texture.id == 0) continue;

        rlActiveTextureSlot(i);

        if (IsCubemapMaterialMap(i)) rlDisableTextureCubemap();
        else rlDisableTexture();
    }

    rlActiveTextureSlot(0);
}

// Upload the classic raylib matrix/color uniforms for the given material shader
static void SetMaterialShaderUniforms(Material material, Matrix matModel)
{
    const int *locs = material.shader.locs;
    if (locs == NULL) return;

    // Upload diffuse/specular colors
    if (locs[SHADER_LOC_COLOR_DIFFUSE] != -1)
    {
        const Color c = material.maps[MATERIAL_MAP_DIFFUSE].color;
        const float values[4] = { (float)c.r/255.0f, (float)c.g/255.0f, (float)c.b/255.0f, (float)c.a/255.0f };
        rlSetUniform(locs[SHADER_LOC_COLOR_DIFFUSE], values, SHADER_UNIFORM_VEC4, 1);
    }

    if (locs[SHADER_LOC_COLOR_SPECULAR] != -1)
    {
        const Color c = material.maps[MATERIAL_MAP_SPECULAR].color;
        const float values[4] = { (float)c.r/255.0f, (float)c.g/255.0f, (float)c.b/255.0f, (float)c.a/255.0f };
        rlSetUniform(locs[SHADER_LOC_COLOR_SPECULAR], values, SHADER_UNIFORM_VEC4, 1);
    }

    const Matrix matView = rlGetMatrixModelview();
    const Matrix matProjection = rlGetMatrixProjection();

    if (locs[SHADER_LOC_MATRIX_VIEW] != -1) rlSetUniformMatrix(locs[SHADER_LOC_MATRIX_VIEW], matView);
    if (locs[SHADER_LOC_MATRIX_PROJECTION] != -1) rlSetUniformMatrix(locs[SHADER_LOC_MATRIX_PROJECTION], matProjection);
    if (locs[SHADER_LOC_MATRIX_MODEL] != -1) rlSetUniformMatrix(locs[SHADER_LOC_MATRIX_MODEL], matModel);
    if (locs[SHADER_LOC_MATRIX_NORMAL] != -1) rlSetUniformMatrix(locs[SHADER_LOC_MATRIX_NORMAL], MatrixTranspose(MatrixInvert(matModel)));

    if (locs[SHADER_LOC_MATRIX_MVP] != -1)
    {
        const Matrix matModelView = MatrixMultiply(matModel, matView);
        rlSetUniformMatrix(locs[SHADER_LOC_MATRIX_MVP], MatrixMultiply(matModelView, matProjection));
    }
}

// Get the index count to feed rlDrawMeshBuffers() for a mesh (0 when non-indexed)
static int GetMeshIndexCount(Mesh mesh)
{
    if ((mesh.indices == NULL) && ((mesh.vboId == NULL) || (mesh.vboId[MESH_VBO_INDICES] == 0))) return 0;
    return mesh.triangleCount*3;
}

// Draw a 3d mesh with material and transform
void DrawMesh(Mesh mesh, Material material, Matrix transform)
{
    DrawMeshInstanced(mesh, material, &transform, 1);
}

// Draw multiple mesh instances with material and different transforms
void DrawMeshInstanced(Mesh mesh, Material material, const Matrix *transforms, int instances)
{
    if ((mesh.vboId == NULL) || (mesh.vertexCount <= 0)) return;
    if ((instances <= 0) || (transforms == NULL)) return;
    if (material.maps == NULL) return;

    // Compose the instance model matrices with the current rlvk transform stack,
    // exactly as raylib does before feeding the model matrix to the shader
    const Matrix matStack = rlGetMatrixTransform();

    Matrix *matInstances = (Matrix *)RL_MALLOC((size_t)instances*sizeof(Matrix));
    if (matInstances == NULL)
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to allocate instance transforms buffer");
        return;
    }

    for (int i = 0; i < instances; i++) matInstances[i] = MatrixMultiply(transforms[i], matStack);

    // NOTE: Only rlEnableShader() is used (like raylib does), rlSetShader() would
    // additionally retarget the immediate-mode batch and leak state to 2D drawing
    rlEnableShader(material.shader.id);

    SetMaterialShaderUniforms(material, matInstances[0]);
    BindMaterialTextures(material);

    rlDrawMeshBuffers(mesh.vboId, mesh.vertexCount, GetMeshIndexCount(mesh), matInstances, instances);

    UnbindMaterialTextures(material);
    rlDisableShader();

    RL_FREE(matInstances);
}

// Upload the skinning palette of a model to its material shader (GPU skinning path)
static void SetModelBoneMatrices(Model model, Material material)
{
    if ((model.boneMatrices == NULL) || (model.skeleton.boneCount == 0)) return;
    if (material.shader.locs == NULL) return;
    if (material.shader.locs[SHADER_LOC_MATRIX_BONETRANSFORMS] == -1) return;

    rlEnableShader(material.shader.id);
    rlSetUniformMatrices(material.shader.locs[SHADER_LOC_MATRIX_BONETRANSFORMS], model.boneMatrices, (int)model.skeleton.boneCount);
}

// Draw a model (with texture if set)
void DrawModel(Model model, Vector3 position, float scale, Color tint)
{
    const Vector3 vScale = { scale, scale, scale };
    const Vector3 rotationAxis = { 0.0f, 1.0f, 0.0f };

    DrawModelEx(model, position, rotationAxis, 0.0f, vScale, tint);
}

// Draw a model with extended parameters
void DrawModelEx(Model model, Vector3 position, Vector3 rotationAxis, float rotationAngle, Vector3 scale, Color tint)
{
    if ((model.meshCount <= 0) || (model.meshes == NULL) || (model.materials == NULL) || (model.meshMaterial == NULL)) return;

    const Matrix matTransform = ComposeModelTransform(position, rotationAxis, rotationAngle, scale);
    model.transform = MatrixMultiply(model.transform, matTransform);

    for (int i = 0; i < model.meshCount; i++)
    {
        const int materialId = model.meshMaterial[i];
        if ((materialId < 0) || (materialId >= model.materialCount)) continue;

        const Color color = model.materials[materialId].maps[MATERIAL_MAP_DIFFUSE].color;

        model.materials[materialId].maps[MATERIAL_MAP_DIFFUSE].color = ColorTint(color, tint);
        SetModelBoneMatrices(model, model.materials[materialId]);
        DrawMesh(model.meshes[i], model.materials[materialId], model.transform);
        model.materials[materialId].maps[MATERIAL_MAP_DIFFUSE].color = color;
    }
}

// Draw a model wires (with texture if set)
void DrawModelWires(Model model, Vector3 position, float scale, Color tint)
{
    rlEnableWireMode();
    DrawModel(model, position, scale, tint);
    rlDisableWireMode();
}

// Draw a model wires (with texture if set) with extended parameters
void DrawModelWiresEx(Model model, Vector3 position, Vector3 rotationAxis, float rotationAngle, Vector3 scale, Color tint)
{
    rlEnableWireMode();
    DrawModelEx(model, position, rotationAxis, rotationAngle, scale, tint);
    rlDisableWireMode();
}

// Draw a bounding box with wires
void DrawBoundingBox(BoundingBox box, Color color)
{
    const Vector3 size = {
        fabsf(box.max.x - box.min.x),
        fabsf(box.max.y - box.min.y),
        fabsf(box.max.z - box.min.z)
    };

    const Vector3 center = {
        box.min.x + size.x/2.0f,
        box.min.y + size.y/2.0f,
        box.min.z + size.z/2.0f
    };

    DrawCubeWires(center, size.x, size.y, size.z, color);
}

// Draw a billboard
void DrawBillboard(Camera camera, Texture2D texture, Vector3 position, float scale, Color tint)
{
    const Rectangle source = { 0.0f, 0.0f, (float)texture.width, (float)texture.height };

    DrawBillboardRec(camera, texture, source, position, (Vector2){ scale, scale }, tint);
}

// Draw a billboard (part of a texture defined by a rectangle)
void DrawBillboardRec(Camera camera, Texture2D texture, Rectangle source, Vector3 position, Vector2 size, Color tint)
{
    // NOTE: Billboard locked on axis-Y
    const Vector3 up = { 0.0f, 1.0f, 0.0f };

    DrawBillboardPro(camera, texture, source, position, up, size, Vector2Scale(size, 0.5f), 0.0f, tint);
}

// Rotate a billboard corner around the billboard plane origin
static Vector3 RotateBillboardCorner(Vector3 corner, Vector3 right, Vector3 up,
                                     float rotateAboutX, float rotateAboutY, float sinRotation, float cosRotation)
{
    const float xtvalue = Vector3DotProduct(right, corner) - rotateAboutX;
    const float ytvalue = Vector3DotProduct(up, corner) - rotateAboutY;
    const float rotatedX = xtvalue*cosRotation - ytvalue*sinRotation + rotateAboutX;
    const float rotatedY = xtvalue*sinRotation + ytvalue*cosRotation + rotateAboutY;

    return Vector3Add(Vector3Scale(up, rotatedY), Vector3Scale(right, rotatedX));
}

// Draw a billboard with additional parameters
void DrawBillboardPro(Camera camera, Texture2D texture, Rectangle source, Vector3 position, Vector3 up, Vector2 size, Vector2 origin, float rotation, Color tint)
{
    if ((texture.width <= 0) || (texture.height <= 0) || (source.height == 0.0f)) return;

    // NOTE: Billboard size will maintain source rectangle aspect ratio, size will represent billboard width
    const Vector2 sizeRatio = { size.x*fabsf(source.width/source.height), size.y };

    const Matrix matView = MatrixLookAt(camera.position, camera.target, camera.up);

    const Vector3 right = { matView.m0, matView.m4, matView.m8 };

    const Vector3 rightScaled = Vector3Scale(right, sizeRatio.x/2.0f);
    const Vector3 upScaled = Vector3Scale(up, sizeRatio.y/2.0f);

    const Vector3 p1 = Vector3Add(rightScaled, upScaled);
    const Vector3 p2 = Vector3Subtract(rightScaled, upScaled);

    Vector3 topLeft = Vector3Scale(p2, -1.0f);
    Vector3 topRight = p1;
    Vector3 bottomRight = p2;
    Vector3 bottomLeft = Vector3Scale(p1, -1.0f);

    if (rotation != 0.0f)
    {
        const float sinRotation = sinf(rotation*DEG2RAD);
        const float cosRotation = cosf(rotation*DEG2RAD);

        // NOTE: (-1, 1) is the range of origin
        const float rotateAboutX = sizeRatio.x*origin.x/2.0f;
        const float rotateAboutY = sizeRatio.y*origin.y/2.0f;

        topLeft = RotateBillboardCorner(topLeft, right, up, rotateAboutX, rotateAboutY, sinRotation, cosRotation);
        topRight = RotateBillboardCorner(topRight, right, up, rotateAboutX, rotateAboutY, sinRotation, cosRotation);
        bottomRight = RotateBillboardCorner(bottomRight, right, up, rotateAboutX, rotateAboutY, sinRotation, cosRotation);
        bottomLeft = RotateBillboardCorner(bottomLeft, right, up, rotateAboutX, rotateAboutY, sinRotation, cosRotation);
    }

    // Translate points to the draw center (position)
    topLeft = Vector3Add(topLeft, position);
    topRight = Vector3Add(topRight, position);
    bottomRight = Vector3Add(bottomRight, position);
    bottomLeft = Vector3Add(bottomLeft, position);

    rlSetTexture(texture.id);

    rlBegin(RL_QUADS);
        rlColor4ub(tint.r, tint.g, tint.b, tint.a);

        // Bottom-left corner for texture and quad
        rlTexCoord2f(source.x/(float)texture.width, source.y/(float)texture.height);
        rlVertex3f(topLeft.x, topLeft.y, topLeft.z);

        // Top-left corner for texture and quad
        rlTexCoord2f(source.x/(float)texture.width, (source.y + source.height)/(float)texture.height);
        rlVertex3f(bottomLeft.x, bottomLeft.y, bottomLeft.z);

        // Top-right corner for texture and quad
        rlTexCoord2f((source.x + source.width)/(float)texture.width, (source.y + source.height)/(float)texture.height);
        rlVertex3f(bottomRight.x, bottomRight.y, bottomRight.z);

        // Bottom-right corner for texture and quad
        rlTexCoord2f((source.x + source.width)/(float)texture.width, source.y/(float)texture.height);
        rlVertex3f(topRight.x, topRight.y, topRight.z);
    rlEnd();

    rlSetTexture(0);
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Mesh utility functions
//----------------------------------------------------------------------------------

// Compute mesh bounding box limits
// NOTE: minVertex and maxVertex should be transformed by model transform matrix
BoundingBox GetMeshBoundingBox(Mesh mesh)
{
    // Get min and max vertex to construct bounds (AABB)
    Vector3 minVertex = { 0 };
    Vector3 maxVertex = { 0 };

    if ((mesh.vertices != NULL) && (mesh.vertexCount > 0))
    {
        minVertex = (Vector3){ mesh.vertices[0], mesh.vertices[1], mesh.vertices[2] };
        maxVertex = minVertex;

        for (int i = 1; i < mesh.vertexCount; i++)
        {
            const Vector3 v = { mesh.vertices[i*3], mesh.vertices[i*3 + 1], mesh.vertices[i*3 + 2] };
            minVertex = Vector3Min(minVertex, v);
            maxVertex = Vector3Max(maxVertex, v);
        }
    }

    const BoundingBox box = { minVertex, maxVertex };

    return box;
}

// Accumulate a triangle contribution into the per-vertex tangent basis accumulators
static void AccumulateTriangleTangents(const Mesh *mesh, int i0, int i1, int i2, Vector3 *tan1, Vector3 *tan2)
{
    const Vector3 v1 = { mesh->vertices[i0*3], mesh->vertices[i0*3 + 1], mesh->vertices[i0*3 + 2] };
    const Vector3 v2 = { mesh->vertices[i1*3], mesh->vertices[i1*3 + 1], mesh->vertices[i1*3 + 2] };
    const Vector3 v3 = { mesh->vertices[i2*3], mesh->vertices[i2*3 + 1], mesh->vertices[i2*3 + 2] };

    const Vector2 uv1 = { mesh->texcoords[i0*2], mesh->texcoords[i0*2 + 1] };
    const Vector2 uv2 = { mesh->texcoords[i1*2], mesh->texcoords[i1*2 + 1] };
    const Vector2 uv3 = { mesh->texcoords[i2*2], mesh->texcoords[i2*2 + 1] };

    const float x1 = v2.x - v1.x, y1 = v2.y - v1.y, z1 = v2.z - v1.z;
    const float x2 = v3.x - v1.x, y2 = v3.y - v1.y, z2 = v3.z - v1.z;

    const float s1 = uv2.x - uv1.x, t1 = uv2.y - uv1.y;
    const float s2 = uv3.x - uv1.x, t2 = uv3.y - uv1.y;

    const float div = s1*t2 - s2*t1;
    const float r = (div == 0.0f)? 0.0f : 1.0f/div;

    const Vector3 sdir = { (t2*x1 - t1*x2)*r, (t2*y1 - t1*y2)*r, (t2*z1 - t1*z2)*r };
    const Vector3 tdir = { (s1*x2 - s2*x1)*r, (s1*y2 - s2*y1)*r, (s1*z2 - s2*z1)*r };

    tan1[i0] = Vector3Add(tan1[i0], sdir);
    tan1[i1] = Vector3Add(tan1[i1], sdir);
    tan1[i2] = Vector3Add(tan1[i2], sdir);

    tan2[i0] = Vector3Add(tan2[i0], tdir);
    tan2[i1] = Vector3Add(tan2[i1], tdir);
    tan2[i2] = Vector3Add(tan2[i2], tdir);
}

// Compute mesh tangents
// NOTE: mesh must have vertices, texcoords and normals; tangents are (re)allocated
void GenMeshTangents(Mesh *mesh)
{
    if (mesh == NULL) return;

    if ((mesh->vertices == NULL) || (mesh->texcoords == NULL) || (mesh->normals == NULL))
    {
        TRACELOG(LOG_WARNING, "MESH: Tangents generation requires texcoord and normal vertex attribute data");
        return;
    }

    RL_FREE(mesh->tangents);
    mesh->tangents = (float *)RL_MALLOC((size_t)mesh->vertexCount*4*sizeof(float));

    Vector3 *tan1 = (Vector3 *)RL_CALLOC((size_t)mesh->vertexCount, sizeof(Vector3));
    Vector3 *tan2 = (Vector3 *)RL_CALLOC((size_t)mesh->vertexCount, sizeof(Vector3));

    if ((mesh->tangents == NULL) || (tan1 == NULL) || (tan2 == NULL))
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to allocate memory required for tangents generation");
        RL_FREE(tan1);
        RL_FREE(tan2);
        RL_FREE(mesh->tangents);
        mesh->tangents = NULL;
        return;
    }

    if (mesh->indices != NULL)
    {
        for (int t = 0; t < mesh->triangleCount; t++)
        {
            AccumulateTriangleTangents(mesh, mesh->indices[t*3], mesh->indices[t*3 + 1], mesh->indices[t*3 + 2], tan1, tan2);
        }
    }
    else
    {
        for (int t = 0; t < mesh->vertexCount/3; t++)
        {
            AccumulateTriangleTangents(mesh, t*3, t*3 + 1, t*3 + 2, tan1, tan2);
        }
    }

    // Gram-Schmidt orthogonalize each tangent against its vertex normal
    for (int i = 0; i < mesh->vertexCount; i++)
    {
        const Vector3 normal = { mesh->normals[i*3], mesh->normals[i*3 + 1], mesh->normals[i*3 + 2] };
        const Vector3 tangent = tan1[i];

        Vector3 ortho = Vector3Normalize(Vector3Subtract(tangent, Vector3Scale(normal, Vector3DotProduct(normal, tangent))));
        if ((ortho.x == 0.0f) && (ortho.y == 0.0f) && (ortho.z == 0.0f)) ortho = (Vector3){ 1.0f, 0.0f, 0.0f };

        mesh->tangents[i*4] = ortho.x;
        mesh->tangents[i*4 + 1] = ortho.y;
        mesh->tangents[i*4 + 2] = ortho.z;

        // Compute the handedness (bitangent sign)
        mesh->tangents[i*4 + 3] = (Vector3DotProduct(Vector3CrossProduct(normal, tangent), tan2[i]) < 0.0f)? -1.0f : 1.0f;
    }

    RL_FREE(tan1);
    RL_FREE(tan2);

    if (mesh->vboId != NULL)
    {
        if (mesh->vboId[MESH_VBO_TANGENT] != 0)
        {
            // Update existing vertex buffer
            rlUpdateVertexBuffer(mesh->vboId[MESH_VBO_TANGENT], mesh->tangents, mesh->vertexCount*4*(int)sizeof(float), 0);
        }
        else
        {
            // Load a new tangent attributes buffer
            mesh->vboId[MESH_VBO_TANGENT] = rlLoadVertexBuffer(mesh->tangents, mesh->vertexCount*4*(int)sizeof(float), false);
        }
    }

    TRACELOG(LOG_INFO, "MESH: Tangents data computed and uploaded for provided mesh");
}

// Export mesh data to file
bool ExportMesh(Mesh mesh, const char *fileName)
{
    bool success = false;

    if ((fileName == NULL) || (mesh.vertices == NULL) || (mesh.vertexCount <= 0)) return false;

    if (IsFileExtension(fileName, ".obj"))
    {
        // Estimated data size, it should be enough
        const size_t dataSize = (size_t)mesh.vertexCount*48 + (size_t)mesh.vertexCount*24 +
                                (size_t)mesh.vertexCount*48 + (size_t)mesh.triangleCount*64 + 2048;

        char *txtData = (char *)RL_CALLOC(dataSize, sizeof(char));
        if (txtData == NULL) return false;

        int byteCount = 0;
        byteCount += sprintf(txtData + byteCount, "# //////////////////////////////////////////////////////////////////////////////////\n");
        byteCount += sprintf(txtData + byteCount, "# //                                                                              //\n");
        byteCount += sprintf(txtData + byteCount, "# // rMeshOBJ exporter v1.0 - Mesh exported as triangle faces and not optimized   //\n");
        byteCount += sprintf(txtData + byteCount, "# //                                                                              //\n");
        byteCount += sprintf(txtData + byteCount, "# // more info and bugs-report:  github.com/raysan5/raylib                        //\n");
        byteCount += sprintf(txtData + byteCount, "# //                                                                              //\n");
        byteCount += sprintf(txtData + byteCount, "# //////////////////////////////////////////////////////////////////////////////////\n\n");
        byteCount += sprintf(txtData + byteCount, "# Vertex Count:     %i\n", mesh.vertexCount);
        byteCount += sprintf(txtData + byteCount, "# Triangle Count:   %i\n\n", mesh.triangleCount);

        byteCount += sprintf(txtData + byteCount, "g mesh\n");

        for (int i = 0; i < mesh.vertexCount*3; i += 3)
        {
            byteCount += sprintf(txtData + byteCount, "v %.6f %.6f %.6f\n", (double)mesh.vertices[i], (double)mesh.vertices[i + 1], (double)mesh.vertices[i + 2]);
        }

        if (mesh.texcoords != NULL)
        {
            for (int i = 0; i < mesh.vertexCount*2; i += 2)
            {
                byteCount += sprintf(txtData + byteCount, "vt %.6f %.6f\n", (double)mesh.texcoords[i], (double)mesh.texcoords[i + 1]);
            }
        }

        if (mesh.normals != NULL)
        {
            for (int i = 0; i < mesh.vertexCount*3; i += 3)
            {
                byteCount += sprintf(txtData + byteCount, "vn %.6f %.6f %.6f\n", (double)mesh.normals[i], (double)mesh.normals[i + 1], (double)mesh.normals[i + 2]);
            }
        }

        if (mesh.indices != NULL)
        {
            for (int i = 0; i < mesh.triangleCount; i++)
            {
                const int a = mesh.indices[i*3] + 1;
                const int b = mesh.indices[i*3 + 1] + 1;
                const int c = mesh.indices[i*3 + 2] + 1;
                byteCount += sprintf(txtData + byteCount, "f %i/%i/%i %i/%i/%i %i/%i/%i\n", a, a, a, b, b, b, c, c, c);
            }
        }
        else
        {
            for (int i = 0, v = 1; i < mesh.triangleCount; i++, v += 3)
            {
                byteCount += sprintf(txtData + byteCount, "f %i/%i/%i %i/%i/%i %i/%i/%i\n",
                                     v, v, v, v + 1, v + 1, v + 1, v + 2, v + 2, v + 2);
            }
        }

        byteCount += sprintf(txtData + byteCount, "\n");

        // NOTE: Text data length exported is determined by '\0' (NULL) character
        success = SaveFileText(fileName, txtData);

        RL_FREE(txtData);
    }
    else if (IsFileExtension(fileName, ".raw"))
    {
        // NOTE: Raw dump of the position attribute only, no header
        success = SaveFileData(fileName, mesh.vertices, mesh.vertexCount*3*(int)sizeof(float));
    }
    else TRACELOG(LOG_WARNING, "MESH: [%s] File format not supported for mesh export", fileName);

    return success;
}

// Write one float array as a C array definition into the export buffer
static int ExportFloatArrayAsCode(char *out, const char *type, const char *name, const char *varName,
                                  const float *data, int count, int perLine)
{
    int byteCount = 0;

    byteCount += sprintf(out + byteCount, "// Mesh %s, %i %s\n", name, count/perLine, name);
    byteCount += sprintf(out + byteCount, "static %s %s[%i] = { ", type, varName, count);

    for (int i = 0; i < count - 1; i++)
    {
        if ((i%perLine) == 0) byteCount += sprintf(out + byteCount, "\n    ");
        byteCount += sprintf(out + byteCount, "%.3ff, ", (double)data[i]);
    }

    byteCount += sprintf(out + byteCount, "%.3ff };\n\n", (double)data[count - 1]);

    return byteCount;
}

// Export mesh as code file (.h) defining multiple arrays of vertex attributes
bool ExportMeshAsCode(Mesh mesh, const char *fileName)
{
    if ((fileName == NULL) || (mesh.vertices == NULL) || (mesh.vertexCount <= 0)) return false;

    // NOTE: Text data buffer size is estimated considering mesh data size
    const size_t dataSize = (size_t)mesh.vertexCount*(3 + 2 + 3 + 4)*16 + (size_t)mesh.triangleCount*3*8 + 4096;

    char *txtData = (char *)RL_CALLOC(dataSize, sizeof(char));
    if (txtData == NULL) return false;

    int byteCount = 0;
    byteCount += sprintf(txtData + byteCount, "////////////////////////////////////////////////////////////////////////////////////////\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                                    //\n");
    byteCount += sprintf(txtData + byteCount, "// MeshAsCode exporter v1.0 - Mesh vertex data exported as arrays                     //\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                                    //\n");
    byteCount += sprintf(txtData + byteCount, "// more info and bugs-report:  github.com/raysan5/raylib                              //\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                                    //\n");
    byteCount += sprintf(txtData + byteCount, "////////////////////////////////////////////////////////////////////////////////////////\n\n");

    // Add image information
    byteCount += sprintf(txtData + byteCount, "// Mesh basic information\n");
    byteCount += sprintf(txtData + byteCount, "#define MESH_VERTEX_COUNT    %i\n", mesh.vertexCount);
    byteCount += sprintf(txtData + byteCount, "#define MESH_TRIANGLE_COUNT  %i\n\n", mesh.triangleCount);

    byteCount += ExportFloatArrayAsCode(txtData + byteCount, "float", "vertices", "meshVertices", mesh.vertices, mesh.vertexCount*3, 3);

    if (mesh.texcoords != NULL)
    {
        byteCount += ExportFloatArrayAsCode(txtData + byteCount, "float", "texcoords", "meshTexcoords", mesh.texcoords, mesh.vertexCount*2, 2);
    }

    if (mesh.normals != NULL)
    {
        byteCount += ExportFloatArrayAsCode(txtData + byteCount, "float", "normals", "meshNormals", mesh.normals, mesh.vertexCount*3, 3);
    }

    if (mesh.tangents != NULL)
    {
        byteCount += ExportFloatArrayAsCode(txtData + byteCount, "float", "tangents", "meshTangents", mesh.tangents, mesh.vertexCount*4, 4);
    }

    if (mesh.colors != NULL)
    {
        const int count = mesh.vertexCount*4;
        byteCount += sprintf(txtData + byteCount, "// Mesh colors, %i colors (RGBA - 32 bit)\n", mesh.vertexCount);
        byteCount += sprintf(txtData + byteCount, "static unsigned char meshColors[%i] = { ", count);
        for (int i = 0; i < count - 1; i++)
        {
            if ((i%4) == 0) byteCount += sprintf(txtData + byteCount, "\n    ");
            byteCount += sprintf(txtData + byteCount, "%i, ", mesh.colors[i]);
        }
        byteCount += sprintf(txtData + byteCount, "%i };\n\n", mesh.colors[count - 1]);
    }

    if (mesh.indices != NULL)
    {
        const int count = mesh.triangleCount*3;
        byteCount += sprintf(txtData + byteCount, "// Mesh indices, %i triangles\n", mesh.triangleCount);
        byteCount += sprintf(txtData + byteCount, "static unsigned short meshIndices[%i] = { ", count);
        for (int i = 0; i < count - 1; i++)
        {
            if ((i%3) == 0) byteCount += sprintf(txtData + byteCount, "\n    ");
            byteCount += sprintf(txtData + byteCount, "%i, ", mesh.indices[i]);
        }
        byteCount += sprintf(txtData + byteCount, "%i };\n", mesh.indices[count - 1]);
    }

    const bool success = SaveFileText(fileName, txtData);

    RL_FREE(txtData);

    return success;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Mesh generation functions
//----------------------------------------------------------------------------------
#if defined(SUPPORT_MESH_GENERATION)

// Build an unindexed raylib Mesh out of a par_shapes mesh (triangles are unrolled)
// NOTE: src must be non-NULL, returned mesh is NOT uploaded to GPU
static Mesh MeshFromParShapes(const par_shapes_mesh *src)
{
    Mesh mesh = { 0 };

    if ((src == NULL) || (src->ntriangles <= 0)) return mesh;

    mesh.vertexCount = src->ntriangles*3;
    mesh.triangleCount = src->ntriangles;

    mesh.vertices = (float *)RL_MALLOC((size_t)mesh.vertexCount*3*sizeof(float));
    mesh.normals = (float *)RL_MALLOC((size_t)mesh.vertexCount*3*sizeof(float));
    mesh.texcoords = (float *)RL_MALLOC((size_t)mesh.vertexCount*2*sizeof(float));

    if ((mesh.vertices == NULL) || (mesh.normals == NULL) || (mesh.texcoords == NULL))
    {
        RL_FREE(mesh.vertices);
        RL_FREE(mesh.normals);
        RL_FREE(mesh.texcoords);

        return (Mesh){ 0 };
    }

    for (int k = 0; k < mesh.vertexCount; k++)
    {
        const int idx = (int)src->triangles[k];

        mesh.vertices[k*3] = src->points[idx*3];
        mesh.vertices[k*3 + 1] = src->points[idx*3 + 1];
        mesh.vertices[k*3 + 2] = src->points[idx*3 + 2];

        if (src->normals != NULL)
        {
            mesh.normals[k*3] = src->normals[idx*3];
            mesh.normals[k*3 + 1] = src->normals[idx*3 + 1];
            mesh.normals[k*3 + 2] = src->normals[idx*3 + 2];
        }
        else
        {
            mesh.normals[k*3] = 0.0f;
            mesh.normals[k*3 + 1] = 1.0f;
            mesh.normals[k*3 + 2] = 0.0f;
        }

        if (src->tcoords != NULL)
        {
            mesh.texcoords[k*2] = src->tcoords[idx*2];
            mesh.texcoords[k*2 + 1] = src->tcoords[idx*2 + 1];
        }
        else
        {
            mesh.texcoords[k*2] = 0.0f;
            mesh.texcoords[k*2 + 1] = 0.0f;
        }
    }

    return mesh;
}

// Fill a par_shapes cap disk with a constant texcoord value (raylib behaviour)
static void ParShapesFillFlatTexcoords(par_shapes_mesh *shape, float value)
{
    if (shape == NULL) return;

    shape->tcoords = PAR_MALLOC(float, 2*shape->npoints);
    if (shape->tcoords == NULL) return;

    for (int i = 0; i < 2*shape->npoints; i++) shape->tcoords[i] = value;
}

// Generate polygonal mesh
Mesh GenMeshPoly(int sides, float radius)
{
    Mesh mesh = { 0 };

    if ((sides < 3) || (radius <= 0.0f))
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: poly");
        return mesh;
    }

    const int vertexCount = sides*3;

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = sides;
    mesh.vertices = (float *)RL_MALLOC((size_t)vertexCount*3*sizeof(float));
    mesh.texcoords = (float *)RL_MALLOC((size_t)vertexCount*2*sizeof(float));
    mesh.normals = (float *)RL_MALLOC((size_t)vertexCount*3*sizeof(float));

    if ((mesh.vertices == NULL) || (mesh.texcoords == NULL) || (mesh.normals == NULL))
    {
        RL_FREE(mesh.vertices);
        RL_FREE(mesh.texcoords);
        RL_FREE(mesh.normals);
        return (Mesh){ 0 };
    }

    const float dStep = 360.0f/(float)sides;

    for (int i = 0; i < sides; i++)
    {
        const float d0 = DEG2RAD*(dStep*(float)i);
        const float d1 = DEG2RAD*(dStep*(float)(i + 1));

        const Vector3 tri[3] = {
            { 0.0f, 0.0f, 0.0f },
            { sinf(d0)*radius, 0.0f, cosf(d0)*radius },
            { sinf(d1)*radius, 0.0f, cosf(d1)*radius }
        };

        for (int k = 0; k < 3; k++)
        {
            const int v = i*3 + k;

            mesh.vertices[v*3] = tri[k].x;
            mesh.vertices[v*3 + 1] = tri[k].y;
            mesh.vertices[v*3 + 2] = tri[k].z;

            mesh.normals[v*3] = 0.0f;
            mesh.normals[v*3 + 1] = 1.0f;
            mesh.normals[v*3 + 2] = 0.0f;

            mesh.texcoords[v*2] = tri[k].x/(2.0f*radius) + 0.5f;
            mesh.texcoords[v*2 + 1] = tri[k].z/(2.0f*radius) + 0.5f;
        }
    }

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate plane mesh (with subdivisions)
Mesh GenMeshPlane(float width, float length, int resX, int resZ)
{
    Mesh mesh = { 0 };

    if ((resX < 1) || (resZ < 1))
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: plane");
        return mesh;
    }

    resX++;
    resZ++;

    const int vertexCount = resX*resZ;
    const int faceCount = (resX - 1)*(resZ - 1);

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = faceCount*2;
    mesh.vertices = (float *)RL_MALLOC((size_t)vertexCount*3*sizeof(float));
    mesh.texcoords = (float *)RL_MALLOC((size_t)vertexCount*2*sizeof(float));
    mesh.normals = (float *)RL_MALLOC((size_t)vertexCount*3*sizeof(float));
    mesh.indices = (unsigned short *)RL_MALLOC((size_t)faceCount*6*sizeof(unsigned short));

    if ((mesh.vertices == NULL) || (mesh.texcoords == NULL) || (mesh.normals == NULL) || (mesh.indices == NULL))
    {
        RL_FREE(mesh.vertices);
        RL_FREE(mesh.texcoords);
        RL_FREE(mesh.normals);
        RL_FREE(mesh.indices);
        return (Mesh){ 0 };
    }

    for (int z = 0; z < resZ; z++)
    {
        const float zPos = ((float)z/(float)(resZ - 1) - 0.5f)*length;

        for (int x = 0; x < resX; x++)
        {
            const float xPos = ((float)x/(float)(resX - 1) - 0.5f)*width;
            const int v = x + z*resX;

            mesh.vertices[v*3] = xPos;
            mesh.vertices[v*3 + 1] = 0.0f;
            mesh.vertices[v*3 + 2] = zPos;

            mesh.normals[v*3] = 0.0f;
            mesh.normals[v*3 + 1] = 1.0f;
            mesh.normals[v*3 + 2] = 0.0f;

            mesh.texcoords[v*2] = (float)x/(float)(resX - 1);
            mesh.texcoords[v*2 + 1] = (float)z/(float)(resZ - 1);
        }
    }

    int t = 0;
    for (int face = 0; face < faceCount; face++)
    {
        // Retrieve lower left corner from face index
        const int i = face%(resX - 1) + (face/(resX - 1))*resX;

        mesh.indices[t++] = (unsigned short)(i + resX);
        mesh.indices[t++] = (unsigned short)(i + 1);
        mesh.indices[t++] = (unsigned short)i;

        mesh.indices[t++] = (unsigned short)(i + resX);
        mesh.indices[t++] = (unsigned short)(i + resX + 1);
        mesh.indices[t++] = (unsigned short)(i + 1);
    }

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate cuboid mesh
Mesh GenMeshCube(float width, float height, float length)
{
    Mesh mesh = { 0 };

    const float vertices[] = {
        -width/2, -height/2, length/2,
         width/2, -height/2, length/2,
         width/2,  height/2, length/2,
        -width/2,  height/2, length/2,
        -width/2, -height/2, -length/2,
        -width/2,  height/2, -length/2,
         width/2,  height/2, -length/2,
         width/2, -height/2, -length/2,
        -width/2,  height/2, -length/2,
        -width/2,  height/2, length/2,
         width/2,  height/2, length/2,
         width/2,  height/2, -length/2,
        -width/2, -height/2, -length/2,
         width/2, -height/2, -length/2,
         width/2, -height/2, length/2,
        -width/2, -height/2, length/2,
         width/2, -height/2, -length/2,
         width/2,  height/2, -length/2,
         width/2,  height/2, length/2,
         width/2, -height/2, length/2,
        -width/2, -height/2, -length/2,
        -width/2, -height/2, length/2,
        -width/2,  height/2, length/2,
        -width/2,  height/2, -length/2
    };

    const float texcoords[] = {
        0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f
    };

    const float normals[] = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f,
        0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f,
        1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f
    };

    mesh.vertices = (float *)RL_MALLOC(24*3*sizeof(float));
    mesh.texcoords = (float *)RL_MALLOC(24*2*sizeof(float));
    mesh.normals = (float *)RL_MALLOC(24*3*sizeof(float));
    mesh.indices = (unsigned short *)RL_MALLOC(36*sizeof(unsigned short));

    if ((mesh.vertices == NULL) || (mesh.texcoords == NULL) || (mesh.normals == NULL) || (mesh.indices == NULL))
    {
        RL_FREE(mesh.vertices);
        RL_FREE(mesh.texcoords);
        RL_FREE(mesh.normals);
        RL_FREE(mesh.indices);
        return (Mesh){ 0 };
    }

    memcpy(mesh.vertices, vertices, 24*3*sizeof(float));
    memcpy(mesh.texcoords, texcoords, 24*2*sizeof(float));
    memcpy(mesh.normals, normals, 24*3*sizeof(float));

    for (int i = 0, k = 0; i < 36; i += 6, k++)
    {
        mesh.indices[i] = (unsigned short)(4*k);
        mesh.indices[i + 1] = (unsigned short)(4*k + 1);
        mesh.indices[i + 2] = (unsigned short)(4*k + 2);
        mesh.indices[i + 3] = (unsigned short)(4*k);
        mesh.indices[i + 4] = (unsigned short)(4*k + 2);
        mesh.indices[i + 5] = (unsigned short)(4*k + 3);
    }

    mesh.vertexCount = 24;
    mesh.triangleCount = 12;

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate sphere mesh (standard sphere)
Mesh GenMeshSphere(float radius, int rings, int slices)
{
    Mesh mesh = { 0 };

    if ((rings < 3) || (slices < 3))
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: sphere");
        return mesh;
    }

    par_shapes_set_epsilon_degenerate_sphere(0.0f);
    par_shapes_mesh *sphere = par_shapes_create_parametric_sphere(slices, rings);
    if (sphere == NULL) return mesh;

    par_shapes_scale(sphere, radius, radius, radius);

    mesh = MeshFromParShapes(sphere);
    par_shapes_free_mesh(sphere);

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate half-sphere mesh (no bottom cap)
Mesh GenMeshHemiSphere(float radius, int rings, int slices)
{
    Mesh mesh = { 0 };

    if ((rings < 3) || (slices < 3))
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: hemisphere");
        return mesh;
    }

    par_shapes_set_epsilon_degenerate_sphere(0.0f);
    par_shapes_mesh *sphere = par_shapes_create_hemisphere(slices, rings);
    if (sphere == NULL) return mesh;

    par_shapes_scale(sphere, radius, radius, radius);

    mesh = MeshFromParShapes(sphere);
    par_shapes_free_mesh(sphere);

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate cylinder mesh
Mesh GenMeshCylinder(float radius, float height, int slices)
{
    Mesh mesh = { 0 };

    if (slices < 3)
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: cylinder");
        return mesh;
    }

    // Instance a cylinder that sits on the Z=0 plane using the given tessellation
    // levels across the UV domain. Think of "slices" like a number of pizza
    // slices, and "stacks" like a number of stacked rings
    // Height and radius are both 1.0, but they can easily be changed with par_shapes_scale()
    par_shapes_mesh *cylinder = par_shapes_create_cylinder(slices, 8);
    if (cylinder == NULL) return mesh;

    par_shapes_scale(cylinder, radius, radius, height);
    par_shapes_rotate(cylinder, -PI/2.0f, (float[]){ 1.0f, 0.0f, 0.0f });

    // Generate an orientable disk shape (top cap)
    par_shapes_mesh *capTop = par_shapes_create_disk(radius, slices, (float[]){ 0.0f, 0.0f, 0.0f }, (float[]){ 0.0f, 0.0f, 1.0f });
    if (capTop != NULL)
    {
        ParShapesFillFlatTexcoords(capTop, 0.0f);
        par_shapes_rotate(capTop, -PI/2.0f, (float[]){ 1.0f, 0.0f, 0.0f });
        par_shapes_translate(capTop, 0.0f, height, 0.0f);
        par_shapes_merge_and_free(cylinder, capTop);
    }

    // Generate an orientable disk shape (bottom cap)
    par_shapes_mesh *capBottom = par_shapes_create_disk(radius, slices, (float[]){ 0.0f, 0.0f, 0.0f }, (float[]){ 0.0f, 0.0f, -1.0f });
    if (capBottom != NULL)
    {
        ParShapesFillFlatTexcoords(capBottom, 0.95f);
        par_shapes_rotate(capBottom, PI/2.0f, (float[]){ 1.0f, 0.0f, 0.0f });
        par_shapes_merge_and_free(cylinder, capBottom);
    }

    mesh = MeshFromParShapes(cylinder);
    par_shapes_free_mesh(cylinder);

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate cone/pyramid mesh
Mesh GenMeshCone(float radius, float height, int slices)
{
    Mesh mesh = { 0 };

    if (slices < 3)
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: cone");
        return mesh;
    }

    // Instance a cone that sits on the Z=0 plane using the given tessellation
    // levels across the UV domain
    par_shapes_mesh *cone = par_shapes_create_cone(slices, 8);
    if (cone == NULL) return mesh;

    par_shapes_scale(cone, radius, radius, height);
    par_shapes_rotate(cone, -PI/2.0f, (float[]){ 1.0f, 0.0f, 0.0f });
    par_shapes_rotate(cone, PI/2.0f, (float[]){ 0.0f, 1.0f, 0.0f });

    // Generate an orientable disk shape (bottom cap)
    par_shapes_mesh *capBottom = par_shapes_create_disk(radius, slices, (float[]){ 0.0f, 0.0f, 0.0f }, (float[]){ 0.0f, 0.0f, -1.0f });
    if (capBottom != NULL)
    {
        ParShapesFillFlatTexcoords(capBottom, 0.95f);
        par_shapes_rotate(capBottom, PI/2.0f, (float[]){ 1.0f, 0.0f, 0.0f });
        par_shapes_merge_and_free(cone, capBottom);
    }

    mesh = MeshFromParShapes(cone);
    par_shapes_free_mesh(cone);

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate torus mesh
Mesh GenMeshTorus(float radius, float size, int radSeg, int sides)
{
    Mesh mesh = { 0 };

    if ((radSeg < 3) || (sides < 3))
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: torus");
        return mesh;
    }

    if (radius > 1.0f) radius = 1.0f;
    else if (radius < 0.1f) radius = 0.1f;

    // Create a donut that sits on the Z=0 plane with the specified inner radius
    par_shapes_mesh *torus = par_shapes_create_torus(radSeg, sides, radius);
    if (torus == NULL) return mesh;

    par_shapes_scale(torus, size/2.0f, size/2.0f, size/2.0f);

    mesh = MeshFromParShapes(torus);
    par_shapes_free_mesh(torus);

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate trefoil knot mesh
Mesh GenMeshKnot(float radius, float size, int radSeg, int sides)
{
    Mesh mesh = { 0 };

    if ((radSeg < 3) || (sides < 3))
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: knot");
        return mesh;
    }

    if (radius > 3.0f) radius = 3.0f;
    else if (radius < 0.5f) radius = 0.5f;

    par_shapes_mesh *knot = par_shapes_create_trefoil_knot(radSeg, sides, radius);
    if (knot == NULL) return mesh;

    par_shapes_scale(knot, size, size, size);

    mesh = MeshFromParShapes(knot);
    par_shapes_free_mesh(knot);

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate a mesh from heightmap
// NOTE: Vertex data is uploaded to GPU
Mesh GenMeshHeightmap(Image heightmap, Vector3 size)
{
    #define GRAY_VALUE(c) ((float)((c).r + (c).g + (c).b)/3.0f)

    Mesh mesh = { 0 };

    const int mapX = heightmap.width;
    const int mapZ = heightmap.height;

    if ((mapX < 2) || (mapZ < 2))
    {
        TRACELOG(LOG_WARNING, "MESH: Failed to generate mesh: heightmap");
        return mesh;
    }

    Color *pixels = LoadImageColors(heightmap);
    if (pixels == NULL) return mesh;

    // NOTE: One vertex per pixel
    mesh.triangleCount = (mapX - 1)*(mapZ - 1)*2;   // One quad every four pixels
    mesh.vertexCount = mesh.triangleCount*3;

    mesh.vertices = (float *)RL_MALLOC((size_t)mesh.vertexCount*3*sizeof(float));
    mesh.normals = (float *)RL_MALLOC((size_t)mesh.vertexCount*3*sizeof(float));
    mesh.texcoords = (float *)RL_MALLOC((size_t)mesh.vertexCount*2*sizeof(float));

    if ((mesh.vertices == NULL) || (mesh.normals == NULL) || (mesh.texcoords == NULL))
    {
        UnloadImageColors(pixels);
        RL_FREE(mesh.vertices);
        RL_FREE(mesh.normals);
        RL_FREE(mesh.texcoords);
        return (Mesh){ 0 };
    }

    int vCounter = 0;       // Used to count vertices float by float
    int tcCounter = 0;      // Used to count texcoords float by float
    int nCounter = 0;       // Used to count normals float by float

    const Vector3 scaleFactor = { size.x/(float)(mapX - 1), size.y/255.0f, size.z/(float)(mapZ - 1) };

    for (int z = 0; z < mapZ - 1; z++)
    {
        for (int x = 0; x < mapX - 1; x++)
        {
            // Fill vertices array with data
            //----------------------------------------------------------
            // One triangle - 3 vertex
            mesh.vertices[vCounter] = (float)x*scaleFactor.x;
            mesh.vertices[vCounter + 1] = GRAY_VALUE(pixels[x + z*mapX])*scaleFactor.y;
            mesh.vertices[vCounter + 2] = (float)z*scaleFactor.z;

            mesh.vertices[vCounter + 3] = (float)x*scaleFactor.x;
            mesh.vertices[vCounter + 4] = GRAY_VALUE(pixels[x + (z + 1)*mapX])*scaleFactor.y;
            mesh.vertices[vCounter + 5] = (float)(z + 1)*scaleFactor.z;

            mesh.vertices[vCounter + 6] = (float)(x + 1)*scaleFactor.x;
            mesh.vertices[vCounter + 7] = GRAY_VALUE(pixels[(x + 1) + z*mapX])*scaleFactor.y;
            mesh.vertices[vCounter + 8] = (float)z*scaleFactor.z;

            // Another triangle - 3 vertex
            mesh.vertices[vCounter + 9] = mesh.vertices[vCounter + 6];
            mesh.vertices[vCounter + 10] = mesh.vertices[vCounter + 7];
            mesh.vertices[vCounter + 11] = mesh.vertices[vCounter + 8];

            mesh.vertices[vCounter + 12] = mesh.vertices[vCounter + 3];
            mesh.vertices[vCounter + 13] = mesh.vertices[vCounter + 4];
            mesh.vertices[vCounter + 14] = mesh.vertices[vCounter + 5];

            mesh.vertices[vCounter + 15] = (float)(x + 1)*scaleFactor.x;
            mesh.vertices[vCounter + 16] = GRAY_VALUE(pixels[(x + 1) + (z + 1)*mapX])*scaleFactor.y;
            mesh.vertices[vCounter + 17] = (float)(z + 1)*scaleFactor.z;
            vCounter += 18;     // 6 vertex, 18 floats

            // Fill texcoords array with data
            //--------------------------------------------------------------
            mesh.texcoords[tcCounter] = (float)x/(float)(mapX - 1);
            mesh.texcoords[tcCounter + 1] = (float)z/(float)(mapZ - 1);

            mesh.texcoords[tcCounter + 2] = (float)x/(float)(mapX - 1);
            mesh.texcoords[tcCounter + 3] = (float)(z + 1)/(float)(mapZ - 1);

            mesh.texcoords[tcCounter + 4] = (float)(x + 1)/(float)(mapX - 1);
            mesh.texcoords[tcCounter + 5] = (float)z/(float)(mapZ - 1);

            mesh.texcoords[tcCounter + 6] = mesh.texcoords[tcCounter + 4];
            mesh.texcoords[tcCounter + 7] = mesh.texcoords[tcCounter + 5];

            mesh.texcoords[tcCounter + 8] = mesh.texcoords[tcCounter + 2];
            mesh.texcoords[tcCounter + 9] = mesh.texcoords[tcCounter + 3];

            mesh.texcoords[tcCounter + 10] = (float)(x + 1)/(float)(mapX - 1);
            mesh.texcoords[tcCounter + 11] = (float)(z + 1)/(float)(mapZ - 1);
            tcCounter += 12;    // 6 texcoords, 12 floats

            // Fill normals array with data
            //--------------------------------------------------------------
            for (int i = 0; i < 18; i += 9)
            {
                const Vector3 vA = { mesh.vertices[nCounter + i], mesh.vertices[nCounter + i + 1], mesh.vertices[nCounter + i + 2] };
                const Vector3 vB = { mesh.vertices[nCounter + i + 3], mesh.vertices[nCounter + i + 4], mesh.vertices[nCounter + i + 5] };
                const Vector3 vC = { mesh.vertices[nCounter + i + 6], mesh.vertices[nCounter + i + 7], mesh.vertices[nCounter + i + 8] };

                const Vector3 vN = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(vB, vA), Vector3Subtract(vC, vA)));

                for (int k = 0; k < 9; k += 3)
                {
                    mesh.normals[nCounter + i + k] = vN.x;
                    mesh.normals[nCounter + i + k + 1] = vN.y;
                    mesh.normals[nCounter + i + k + 2] = vN.z;
                }
            }

            nCounter += 18;     // 6 vertex, 18 floats
        }
    }

    UnloadImageColors(pixels);

    UploadMesh(&mesh, false);

    return mesh;
}

// Generate a cubes mesh from pixel data
// NOTE: Vertex data is uploaded to GPU
Mesh GenMeshCubicmap(Image cubicmap, Vector3 cubeSize)
{
    #define COLOR_EQUAL(col1, col2) ((col1.r == col2.r) && (col1.g == col2.g) && (col1.b == col2.b) && (col1.a == col2.a))

    Mesh mesh = { 0 };

    Color *pixels = LoadImageColors(cubicmap);
    if (pixels == NULL) return mesh;

    // NOTE: Max possible number of triangles numCubes*(12 triangles by cube)
    const int maxTriangles = cubicmap.width*cubicmap.height*12;

    int vCounter = 0;       // Used to count vertices
    int tcCounter = 0;      // Used to count texcoords
    int nCounter = 0;       // Used to count normals

    const float w = cubeSize.x;
    const float h = cubeSize.z;
    const float h2 = cubeSize.y;

    Vector3 *mapVertices = (Vector3 *)RL_MALLOC((size_t)maxTriangles*3*sizeof(Vector3));
    Vector2 *mapTexcoords = (Vector2 *)RL_MALLOC((size_t)maxTriangles*3*sizeof(Vector2));
    Vector3 *mapNormals = (Vector3 *)RL_MALLOC((size_t)maxTriangles*3*sizeof(Vector3));

    if ((mapVertices == NULL) || (mapTexcoords == NULL) || (mapNormals == NULL))
    {
        UnloadImageColors(pixels);
        RL_FREE(mapVertices);
        RL_FREE(mapTexcoords);
        RL_FREE(mapNormals);
        return mesh;
    }

    // Define the 6 normals of the cube, we will combine them accordingly later
    const Vector3 n1 = { 1.0f, 0.0f, 0.0f };
    const Vector3 n2 = { -1.0f, 0.0f, 0.0f };
    const Vector3 n3 = { 0.0f, 1.0f, 0.0f };
    const Vector3 n4 = { 0.0f, -1.0f, 0.0f };
    const Vector3 n5 = { 0.0f, 0.0f, -1.0f };
    const Vector3 n6 = { 0.0f, 0.0f, 1.0f };

    // NOTE: We use texture rectangles to define different textures for top-bottom-front-back-right-left (6)
    typedef struct RectangleF {
        float x, y, width, height;
    } RectangleF;

    const RectangleF rightTexUV = { 0.0f, 0.0f, 0.5f, 0.5f };
    const RectangleF leftTexUV = { 0.5f, 0.0f, 0.5f, 0.5f };
    const RectangleF frontTexUV = { 0.0f, 0.0f, 0.5f, 0.5f };
    const RectangleF backTexUV = { 0.5f, 0.0f, 0.5f, 0.5f };
    const RectangleF topTexUV = { 0.0f, 0.5f, 0.5f, 0.5f };
    const RectangleF bottomTexUV = { 0.5f, 0.5f, 0.5f, 0.5f };

    for (int z = 0; z < cubicmap.height; ++z)
    {
        for (int x = 0; x < cubicmap.width; ++x)
        {
            // Define the 8 vertex of the cube, we will combine them accordingly later
            const Vector3 v1 = { w*(x - 0.5f), h2, h*(z - 0.5f) };
            const Vector3 v2 = { w*(x - 0.5f), h2, h*(z + 0.5f) };
            const Vector3 v3 = { w*(x + 0.5f), h2, h*(z + 0.5f) };
            const Vector3 v4 = { w*(x + 0.5f), h2, h*(z - 0.5f) };
            const Vector3 v5 = { w*(x + 0.5f), 0.0f, h*(z - 0.5f) };
            const Vector3 v6 = { w*(x - 0.5f), 0.0f, h*(z - 0.5f) };
            const Vector3 v7 = { w*(x - 0.5f), 0.0f, h*(z + 0.5f) };
            const Vector3 v8 = { w*(x + 0.5f), 0.0f, h*(z + 0.5f) };

            const Color cubicmapColor = pixels[z*cubicmap.width + x];
            const Color white = { 255, 255, 255, 255 };
            const Color black = { 0, 0, 0, 255 };

            if (COLOR_EQUAL(cubicmapColor, white))      // White pixel, it's a full cube
            {
                // Define triangles and checking collateral cubes
                //------------------------------------------------

                // Define top triangles (2 tris, 6 vertex --> v1-v2-v3, v1-v3-v4)
                mapVertices[vCounter] = v1;
                mapVertices[vCounter + 1] = v2;
                mapVertices[vCounter + 2] = v3;
                mapVertices[vCounter + 3] = v1;
                mapVertices[vCounter + 4] = v3;
                mapVertices[vCounter + 5] = v4;
                vCounter += 6;

                mapNormals[nCounter] = n3;
                mapNormals[nCounter + 1] = n3;
                mapNormals[nCounter + 2] = n3;
                mapNormals[nCounter + 3] = n3;
                mapNormals[nCounter + 4] = n3;
                mapNormals[nCounter + 5] = n3;
                nCounter += 6;

                mapTexcoords[tcCounter] = (Vector2){ topTexUV.x, topTexUV.y };
                mapTexcoords[tcCounter + 1] = (Vector2){ topTexUV.x, topTexUV.y + topTexUV.height };
                mapTexcoords[tcCounter + 2] = (Vector2){ topTexUV.x + topTexUV.width, topTexUV.y + topTexUV.height };
                mapTexcoords[tcCounter + 3] = (Vector2){ topTexUV.x, topTexUV.y };
                mapTexcoords[tcCounter + 4] = (Vector2){ topTexUV.x + topTexUV.width, topTexUV.y + topTexUV.height };
                mapTexcoords[tcCounter + 5] = (Vector2){ topTexUV.x + topTexUV.width, topTexUV.y };
                tcCounter += 6;

                // Define bottom triangles (2 tris, 6 vertex --> v6-v8-v7, v6-v5-v8)
                mapVertices[vCounter] = v6;
                mapVertices[vCounter + 1] = v8;
                mapVertices[vCounter + 2] = v7;
                mapVertices[vCounter + 3] = v6;
                mapVertices[vCounter + 4] = v5;
                mapVertices[vCounter + 5] = v8;
                vCounter += 6;

                mapNormals[nCounter] = n4;
                mapNormals[nCounter + 1] = n4;
                mapNormals[nCounter + 2] = n4;
                mapNormals[nCounter + 3] = n4;
                mapNormals[nCounter + 4] = n4;
                mapNormals[nCounter + 5] = n4;
                nCounter += 6;

                mapTexcoords[tcCounter] = (Vector2){ bottomTexUV.x + bottomTexUV.width, bottomTexUV.y };
                mapTexcoords[tcCounter + 1] = (Vector2){ bottomTexUV.x, bottomTexUV.y + bottomTexUV.height };
                mapTexcoords[tcCounter + 2] = (Vector2){ bottomTexUV.x + bottomTexUV.width, bottomTexUV.y + bottomTexUV.height };
                mapTexcoords[tcCounter + 3] = (Vector2){ bottomTexUV.x + bottomTexUV.width, bottomTexUV.y };
                mapTexcoords[tcCounter + 4] = (Vector2){ bottomTexUV.x, bottomTexUV.y };
                mapTexcoords[tcCounter + 5] = (Vector2){ bottomTexUV.x, bottomTexUV.y + bottomTexUV.height };
                tcCounter += 6;

                // Checking cube on bottom of current cube
                if (((z < cubicmap.height - 1) && COLOR_EQUAL(pixels[(z + 1)*cubicmap.width + x], black)) || (z == cubicmap.height - 1))
                {
                    // Define front triangles (2 tris, 6 vertex) --> v2 v7 v3, v3 v7 v8
                    // NOTE: Collateral occluded faces are not generated
                    mapVertices[vCounter] = v2;
                    mapVertices[vCounter + 1] = v7;
                    mapVertices[vCounter + 2] = v3;
                    mapVertices[vCounter + 3] = v3;
                    mapVertices[vCounter + 4] = v7;
                    mapVertices[vCounter + 5] = v8;
                    vCounter += 6;

                    mapNormals[nCounter] = n6;
                    mapNormals[nCounter + 1] = n6;
                    mapNormals[nCounter + 2] = n6;
                    mapNormals[nCounter + 3] = n6;
                    mapNormals[nCounter + 4] = n6;
                    mapNormals[nCounter + 5] = n6;
                    nCounter += 6;

                    mapTexcoords[tcCounter] = (Vector2){ frontTexUV.x, frontTexUV.y };
                    mapTexcoords[tcCounter + 1] = (Vector2){ frontTexUV.x, frontTexUV.y + frontTexUV.height };
                    mapTexcoords[tcCounter + 2] = (Vector2){ frontTexUV.x + frontTexUV.width, frontTexUV.y };
                    mapTexcoords[tcCounter + 3] = (Vector2){ frontTexUV.x + frontTexUV.width, frontTexUV.y };
                    mapTexcoords[tcCounter + 4] = (Vector2){ frontTexUV.x, frontTexUV.y + frontTexUV.height };
                    mapTexcoords[tcCounter + 5] = (Vector2){ frontTexUV.x + frontTexUV.width, frontTexUV.y + frontTexUV.height };
                    tcCounter += 6;
                }

                // Checking cube on top of current cube
                if (((z > 0) && COLOR_EQUAL(pixels[(z - 1)*cubicmap.width + x], black)) || (z == 0))
                {
                    // Define back triangles (2 tris, 6 vertex) --> v1 v5 v6, v1 v4 v5
                    mapVertices[vCounter] = v1;
                    mapVertices[vCounter + 1] = v5;
                    mapVertices[vCounter + 2] = v6;
                    mapVertices[vCounter + 3] = v1;
                    mapVertices[vCounter + 4] = v4;
                    mapVertices[vCounter + 5] = v5;
                    vCounter += 6;

                    mapNormals[nCounter] = n5;
                    mapNormals[nCounter + 1] = n5;
                    mapNormals[nCounter + 2] = n5;
                    mapNormals[nCounter + 3] = n5;
                    mapNormals[nCounter + 4] = n5;
                    mapNormals[nCounter + 5] = n5;
                    nCounter += 6;

                    mapTexcoords[tcCounter] = (Vector2){ backTexUV.x + backTexUV.width, backTexUV.y };
                    mapTexcoords[tcCounter + 1] = (Vector2){ backTexUV.x, backTexUV.y + backTexUV.height };
                    mapTexcoords[tcCounter + 2] = (Vector2){ backTexUV.x + backTexUV.width, backTexUV.y + backTexUV.height };
                    mapTexcoords[tcCounter + 3] = (Vector2){ backTexUV.x + backTexUV.width, backTexUV.y };
                    mapTexcoords[tcCounter + 4] = (Vector2){ backTexUV.x, backTexUV.y };
                    mapTexcoords[tcCounter + 5] = (Vector2){ backTexUV.x, backTexUV.y + backTexUV.height };
                    tcCounter += 6;
                }

                // Checking cube on right of current cube
                if (((x < cubicmap.width - 1) && COLOR_EQUAL(pixels[z*cubicmap.width + (x + 1)], black)) || (x == cubicmap.width - 1))
                {
                    // Define right triangles (2 tris, 6 vertex) --> v3 v8 v4, v4 v8 v5
                    mapVertices[vCounter] = v3;
                    mapVertices[vCounter + 1] = v8;
                    mapVertices[vCounter + 2] = v4;
                    mapVertices[vCounter + 3] = v4;
                    mapVertices[vCounter + 4] = v8;
                    mapVertices[vCounter + 5] = v5;
                    vCounter += 6;

                    mapNormals[nCounter] = n1;
                    mapNormals[nCounter + 1] = n1;
                    mapNormals[nCounter + 2] = n1;
                    mapNormals[nCounter + 3] = n1;
                    mapNormals[nCounter + 4] = n1;
                    mapNormals[nCounter + 5] = n1;
                    nCounter += 6;

                    mapTexcoords[tcCounter] = (Vector2){ rightTexUV.x, rightTexUV.y };
                    mapTexcoords[tcCounter + 1] = (Vector2){ rightTexUV.x, rightTexUV.y + rightTexUV.height };
                    mapTexcoords[tcCounter + 2] = (Vector2){ rightTexUV.x + rightTexUV.width, rightTexUV.y };
                    mapTexcoords[tcCounter + 3] = (Vector2){ rightTexUV.x + rightTexUV.width, rightTexUV.y };
                    mapTexcoords[tcCounter + 4] = (Vector2){ rightTexUV.x, rightTexUV.y + rightTexUV.height };
                    mapTexcoords[tcCounter + 5] = (Vector2){ rightTexUV.x + rightTexUV.width, rightTexUV.y + rightTexUV.height };
                    tcCounter += 6;
                }

                // Checking cube on left of current cube
                if (((x > 0) && COLOR_EQUAL(pixels[z*cubicmap.width + (x - 1)], black)) || (x == 0))
                {
                    // Define left triangles (2 tris, 6 vertex) --> v1 v7 v2, v1 v6 v7
                    mapVertices[vCounter] = v1;
                    mapVertices[vCounter + 1] = v7;
                    mapVertices[vCounter + 2] = v2;
                    mapVertices[vCounter + 3] = v1;
                    mapVertices[vCounter + 4] = v6;
                    mapVertices[vCounter + 5] = v7;
                    vCounter += 6;

                    mapNormals[nCounter] = n2;
                    mapNormals[nCounter + 1] = n2;
                    mapNormals[nCounter + 2] = n2;
                    mapNormals[nCounter + 3] = n2;
                    mapNormals[nCounter + 4] = n2;
                    mapNormals[nCounter + 5] = n2;
                    nCounter += 6;

                    mapTexcoords[tcCounter] = (Vector2){ leftTexUV.x, leftTexUV.y };
                    mapTexcoords[tcCounter + 1] = (Vector2){ leftTexUV.x + leftTexUV.width, leftTexUV.y + leftTexUV.height };
                    mapTexcoords[tcCounter + 2] = (Vector2){ leftTexUV.x + leftTexUV.width, leftTexUV.y };
                    mapTexcoords[tcCounter + 3] = (Vector2){ leftTexUV.x, leftTexUV.y };
                    mapTexcoords[tcCounter + 4] = (Vector2){ leftTexUV.x, leftTexUV.y + leftTexUV.height };
                    mapTexcoords[tcCounter + 5] = (Vector2){ leftTexUV.x + leftTexUV.width, leftTexUV.y + leftTexUV.height };
                    tcCounter += 6;
                }
            }
            else if (COLOR_EQUAL(cubicmapColor, black))  // Black pixel, it's a hole
            {
                // Define top triangles (2 tris, 6 vertex --> v1-v3-v2, v1-v4-v3)
                mapVertices[vCounter] = v1;
                mapVertices[vCounter + 1] = v3;
                mapVertices[vCounter + 2] = v2;
                mapVertices[vCounter + 3] = v1;
                mapVertices[vCounter + 4] = v4;
                mapVertices[vCounter + 5] = v3;
                vCounter += 6;

                mapNormals[nCounter] = n4;
                mapNormals[nCounter + 1] = n4;
                mapNormals[nCounter + 2] = n4;
                mapNormals[nCounter + 3] = n4;
                mapNormals[nCounter + 4] = n4;
                mapNormals[nCounter + 5] = n4;
                nCounter += 6;

                mapTexcoords[tcCounter] = (Vector2){ topTexUV.x, topTexUV.y };
                mapTexcoords[tcCounter + 1] = (Vector2){ topTexUV.x + topTexUV.width, topTexUV.y + topTexUV.height };
                mapTexcoords[tcCounter + 2] = (Vector2){ topTexUV.x, topTexUV.y + topTexUV.height };
                mapTexcoords[tcCounter + 3] = (Vector2){ topTexUV.x, topTexUV.y };
                mapTexcoords[tcCounter + 4] = (Vector2){ topTexUV.x + topTexUV.width, topTexUV.y };
                mapTexcoords[tcCounter + 5] = (Vector2){ topTexUV.x + topTexUV.width, topTexUV.y + topTexUV.height };
                tcCounter += 6;

                // Define bottom triangles (2 tris, 6 vertex --> v6-v7-v8, v6-v8-v5)
                mapVertices[vCounter] = v6;
                mapVertices[vCounter + 1] = v7;
                mapVertices[vCounter + 2] = v8;
                mapVertices[vCounter + 3] = v6;
                mapVertices[vCounter + 4] = v8;
                mapVertices[vCounter + 5] = v5;
                vCounter += 6;

                mapNormals[nCounter] = n3;
                mapNormals[nCounter + 1] = n3;
                mapNormals[nCounter + 2] = n3;
                mapNormals[nCounter + 3] = n3;
                mapNormals[nCounter + 4] = n3;
                mapNormals[nCounter + 5] = n3;
                nCounter += 6;

                mapTexcoords[tcCounter] = (Vector2){ bottomTexUV.x + bottomTexUV.width, bottomTexUV.y };
                mapTexcoords[tcCounter + 1] = (Vector2){ bottomTexUV.x + bottomTexUV.width, bottomTexUV.y + bottomTexUV.height };
                mapTexcoords[tcCounter + 2] = (Vector2){ bottomTexUV.x, bottomTexUV.y + bottomTexUV.height };
                mapTexcoords[tcCounter + 3] = (Vector2){ bottomTexUV.x + bottomTexUV.width, bottomTexUV.y };
                mapTexcoords[tcCounter + 4] = (Vector2){ bottomTexUV.x, bottomTexUV.y + bottomTexUV.height };
                mapTexcoords[tcCounter + 5] = (Vector2){ bottomTexUV.x, bottomTexUV.y };
                tcCounter += 6;
            }
        }
    }

    // Move data from mapVertices temp arrays to vertices float array
    mesh.vertexCount = vCounter;
    mesh.triangleCount = vCounter/3;

    mesh.vertices = (float *)RL_MALLOC((size_t)mesh.vertexCount*3*sizeof(float));
    mesh.normals = (float *)RL_MALLOC((size_t)mesh.vertexCount*3*sizeof(float));
    mesh.texcoords = (float *)RL_MALLOC((size_t)mesh.vertexCount*2*sizeof(float));

    if ((mesh.vertices == NULL) || (mesh.normals == NULL) || (mesh.texcoords == NULL))
    {
        RL_FREE(mesh.vertices);
        RL_FREE(mesh.normals);
        RL_FREE(mesh.texcoords);
        mesh = (Mesh){ 0 };
    }
    else
    {
        for (int i = 0; i < mesh.vertexCount; i++)
        {
            mesh.vertices[i*3] = mapVertices[i].x;
            mesh.vertices[i*3 + 1] = mapVertices[i].y;
            mesh.vertices[i*3 + 2] = mapVertices[i].z;

            mesh.normals[i*3] = mapNormals[i].x;
            mesh.normals[i*3 + 1] = mapNormals[i].y;
            mesh.normals[i*3 + 2] = mapNormals[i].z;

            mesh.texcoords[i*2] = mapTexcoords[i].x;
            mesh.texcoords[i*2 + 1] = mapTexcoords[i].y;
        }
    }

    RL_FREE(mapVertices);
    RL_FREE(mapNormals);
    RL_FREE(mapTexcoords);

    UnloadImageColors(pixels);

    UploadMesh(&mesh, false);

    return mesh;
}

#endif      // SUPPORT_MESH_GENERATION

//----------------------------------------------------------------------------------
// Module Functions Definition - Material loading/unloading functions
//----------------------------------------------------------------------------------

// Load a texture referenced by a model file, relative to the model directory
// NOTE: Returns a zeroed texture when the file can not be loaded
static Texture2D LoadTextureRelative(const char *baseDir, const char *fileName)
{
    Texture2D texture = { 0 };

    if ((fileName == NULL) || (fileName[0] == '\0')) return texture;

    if ((baseDir != NULL) && (baseDir[0] != '\0')) texture = LoadTexture(TextFormat("%s/%s", baseDir, fileName));
    else texture = LoadTexture(fileName);

    return texture;
}

#if defined(SUPPORT_FILEFORMAT_MTL) || defined(SUPPORT_FILEFORMAT_OBJ)
// Fill a raylib material array out of the tinyobj material array
static void ProcessMaterialsOBJ(Material *materials, const tinyobj_material_t *mats, int materialCount, const char *baseDir)
{
    for (int m = 0; m < materialCount; m++)
    {
        materials[m] = LoadMaterialDefault();
        if (materials[m].maps == NULL) continue;

        const tinyobj_material_t *src = &mats[m];

        // Albedo/diffuse
        const Texture2D diffuse = LoadTextureRelative(baseDir, src->diffuse_texname);
        if (diffuse.id > 0) materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = diffuse;

        materials[m].maps[MATERIAL_MAP_DIFFUSE].color = (Color){
            (unsigned char)(src->diffuse[0]*255.0f),
            (unsigned char)(src->diffuse[1]*255.0f),
            (unsigned char)(src->diffuse[2]*255.0f), 255 };
        materials[m].maps[MATERIAL_MAP_DIFFUSE].value = 0.0f;

        // Specular
        const Texture2D specular = LoadTextureRelative(baseDir, src->specular_texname);
        if (specular.id > 0) materials[m].maps[MATERIAL_MAP_SPECULAR].texture = specular;

        materials[m].maps[MATERIAL_MAP_SPECULAR].color = (Color){
            (unsigned char)(src->specular[0]*255.0f),
            (unsigned char)(src->specular[1]*255.0f),
            (unsigned char)(src->specular[2]*255.0f), 255 };
        materials[m].maps[MATERIAL_MAP_SPECULAR].value = src->shininess;

        // Normal/bump
        const Texture2D normal = LoadTextureRelative(baseDir, src->bump_texname);
        if (normal.id > 0) materials[m].maps[MATERIAL_MAP_NORMAL].texture = normal;

        materials[m].maps[MATERIAL_MAP_NORMAL].color = WHITE;
        materials[m].maps[MATERIAL_MAP_NORMAL].value = src->shininess;

        // Emission
        materials[m].maps[MATERIAL_MAP_EMISSION].color = (Color){
            (unsigned char)(src->emission[0]*255.0f),
            (unsigned char)(src->emission[1]*255.0f),
            (unsigned char)(src->emission[2]*255.0f), 255 };

        // Height/displacement
        const Texture2D height = LoadTextureRelative(baseDir, src->displacement_texname);
        if (height.id > 0) materials[m].maps[MATERIAL_MAP_HEIGHT].texture = height;
    }
}
#endif

// Load materials from model file
Material *LoadMaterials(const char *fileName, int *materialCount)
{
    Material *materials = NULL;
    unsigned int count = 0;

    if (fileName == NULL)
    {
        if (materialCount != NULL) *materialCount = 0;
        return NULL;
    }

#if defined(SUPPORT_FILEFORMAT_MTL)
    if (IsFileExtension(fileName, ".mtl"))
    {
        tinyobj_material_t *mats = NULL;

        const int result = tinyobj_parse_mtl_file(&mats, &count, fileName);
        if (result != TINYOBJ_SUCCESS) TRACELOG(LOG_WARNING, "MATERIAL: [%s] Failed to parse materials file", fileName);

        if ((mats != NULL) && (count > 0))
        {
            materials = (Material *)RL_CALLOC(count, sizeof(Material));

            if (materials != NULL) ProcessMaterialsOBJ(materials, mats, (int)count, GetDirectoryPath(fileName));
            else count = 0;
        }
        else count = 0;

        tinyobj_materials_free(mats, count);
    }
    else
#endif
    TRACELOG(LOG_WARNING, "MATERIAL: [%s] Failed to load material file, format not supported", fileName);

    if (materialCount != NULL) *materialCount = (int)count;

    return materials;
}

// Load default material (Supports: DIFFUSE, SPECULAR, NORMAL maps)
Material LoadMaterialDefault(void)
{
    Material material = { 0 };

    material.maps = (MaterialMap *)RL_CALLOC(MAX_MATERIAL_MAPS, sizeof(MaterialMap));
    if (material.maps == NULL)
    {
        TRACELOG(LOG_WARNING, "MATERIAL: Failed to allocate material maps array");
        return material;
    }

    // Using rlvk default shader
    material.shader.id = rlGetShaderIdDefault();
    material.shader.locs = rlGetShaderLocsDefault();

    // Using rlvk default texture (1x1 pixel, UNCOMPRESSED_R8G8B8A8, 1 mipmap)
    material.maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){ rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };

    material.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;      // Diffuse color
    material.maps[MATERIAL_MAP_SPECULAR].color = WHITE;     // Specular color

    return material;
}

// Check if a material is valid (map textures loaded in GPU)
bool IsMaterialValid(Material material)
{
    if (material.maps == NULL) return false;
    if (material.shader.id == 0) return false;

    // Check if material contains at least one valid map texture
    for (int i = 0; i < MAX_MATERIAL_MAPS; i++)
    {
        if (material.maps[i].texture.id > 0) return true;
    }

    return false;
}

// Unload material from memory
void UnloadMaterial(Material material)
{
    // Unload material shader (avoid unloading default shader, managed by rlvk)
    if (material.shader.id != rlGetShaderIdDefault()) UnloadShader(material.shader);

    // Unload loaded texture maps (avoid unloading default texture, managed by rlvk)
    if (material.maps != NULL)
    {
        for (int i = 0; i < MAX_MATERIAL_MAPS; i++)
        {
            if (material.maps[i].texture.id != rlGetTextureIdDefault()) rlUnloadTexture(material.maps[i].texture.id);
        }

        RL_FREE(material.maps);
    }
}

// Set texture for a material map type (MATERIAL_MAP_DIFFUSE, MATERIAL_MAP_SPECULAR...)
// NOTE: Previous texture should be manually unloaded
void SetMaterialTexture(Material *material, int mapType, Texture2D texture)
{
    if ((material == NULL) || (material->maps == NULL)) return;
    if ((mapType < 0) || (mapType >= MAX_MATERIAL_MAPS)) return;

    material->maps[mapType].texture = texture;
}

// Set material for a mesh
void SetModelMeshMaterial(Model *model, int meshId, int materialId)
{
    if (model == NULL) return;

    if ((meshId < 0) || (meshId >= model->meshCount)) TRACELOG(LOG_WARNING, "MESH: Id greater than mesh count");
    else if ((materialId < 0) || (materialId >= model->materialCount)) TRACELOG(LOG_WARNING, "MATERIAL: Id greater than material count");
    else if (model->meshMaterial != NULL) model->meshMaterial[meshId] = materialId;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Model animations
//----------------------------------------------------------------------------------

// Build the absolute (model space) bind pose out of the parent-relative bone transforms
static void BuildPoseFromParentJoints(const BoneInfo *bones, unsigned int boneCount, Transform *transforms)
{
    if ((bones == NULL) || (transforms == NULL)) return;

    for (unsigned int i = 0; i < boneCount; i++)
    {
        if (bones[i].parent < 0) continue;
        if (bones[i].parent > (int)i)
        {
            TRACELOG(LOG_WARNING, "BONE: Assumes bones are toplogically sorted, but bone %d has parent %d. Skipping.", i, bones[i].parent);
            continue;
        }

        const Transform parent = transforms[bones[i].parent];

        transforms[i].rotation = QuaternionMultiply(parent.rotation, transforms[i].rotation);
        transforms[i].translation = Vector3RotateByQuaternion(transforms[i].translation, parent.rotation);
        transforms[i].translation = Vector3Add(transforms[i].translation, parent.translation);
        transforms[i].scale = Vector3Multiply(transforms[i].scale, parent.scale);
    }
}

// Allocate the runtime pose/palette arrays of a freshly loaded skinned model
static void SkeletonAllocDefaultPose(Model *model)
{
    if ((model == NULL) || (model->skeleton.boneCount == 0)) return;
    if (model->skeleton.bindPose == NULL) return;

    const unsigned int boneCount = model->skeleton.boneCount;

    if (model->currentPose == NULL)
    {
        model->currentPose = (Transform *)RL_MALLOC((size_t)boneCount*sizeof(Transform));
        if (model->currentPose != NULL) memcpy(model->currentPose, model->skeleton.bindPose, (size_t)boneCount*sizeof(Transform));
    }

    if (model->boneMatrices == NULL)
    {
        model->boneMatrices = (Matrix *)RL_MALLOC((size_t)boneCount*sizeof(Matrix));

        if (model->boneMatrices != NULL)
        {
            for (unsigned int i = 0; i < boneCount; i++) model->boneMatrices[i] = MatrixIdentity();
        }
    }
}

// Sample an animation pose at a (possibly fractional) keyframe position
// NOTE: out must hold at least anim.boneCount transforms
static void EvaluateAnimationPose(ModelAnimation anim, float frame, Transform *out)
{
    if ((anim.keyframePoses == NULL) || (anim.keyframeCount <= 0) || (out == NULL)) return;

    if (frame < 0.0f) frame = 0.0f;

    const float wrapped = fmodf(frame, (float)anim.keyframeCount);
    const int f0 = (int)wrapped;
    const int f1 = (f0 + 1)%anim.keyframeCount;
    const float t = wrapped - (float)f0;

    const Transform *poseA = anim.keyframePoses[f0];
    const Transform *poseB = anim.keyframePoses[f1];

    if ((poseA == NULL) || (poseB == NULL)) return;

    for (unsigned int i = 0; i < anim.boneCount; i++)
    {
        out[i].translation = Vector3Lerp(poseA[i].translation, poseB[i].translation, t);
        out[i].rotation = QuaternionSlerp(poseA[i].rotation, poseB[i].rotation, t);
        out[i].scale = Vector3Lerp(poseA[i].scale, poseB[i].scale, t);
    }
}

// Recompute the skinning palette from the model bind pose and its current pose
static void UpdateModelBoneMatrices(Model model)
{
    if ((model.boneMatrices == NULL) || (model.currentPose == NULL) || (model.skeleton.bindPose == NULL)) return;

    for (unsigned int boneId = 0; boneId < model.skeleton.boneCount; boneId++)
    {
        const Transform in = model.skeleton.bindPose[boneId];
        const Transform out = model.currentPose[boneId];

        const Quaternion invRotation = QuaternionInvert(in.rotation);
        const Vector3 invTranslation = Vector3RotateByQuaternion(Vector3Negate(in.translation), invRotation);
        const Vector3 invScale = {
            (in.scale.x != 0.0f)? 1.0f/in.scale.x : 0.0f,
            (in.scale.y != 0.0f)? 1.0f/in.scale.y : 0.0f,
            (in.scale.z != 0.0f)? 1.0f/in.scale.z : 0.0f
        };

        const Vector3 boneTranslation = Vector3Add(
            Vector3RotateByQuaternion(Vector3Multiply(out.scale, invTranslation), out.rotation), out.translation);
        const Quaternion boneRotation = QuaternionMultiply(out.rotation, invRotation);
        const Vector3 boneScale = Vector3Multiply(out.scale, invScale);

        model.boneMatrices[boneId] = MatrixMultiply(MatrixMultiply(
            MatrixScale(boneScale.x, boneScale.y, boneScale.z),
            QuaternionToMatrix(boneRotation)),
            MatrixTranslate(boneTranslation.x, boneTranslation.y, boneTranslation.z));
    }
}

// Apply CPU skinning to a mesh and push the result to its vertex buffers
static void UpdateMeshSkinning(Mesh *mesh, const Matrix *boneMatrices, unsigned int boneCount)
{
    if ((mesh->boneIndices == NULL) || (mesh->boneWeights == NULL)) return;
    if ((mesh->animVertices == NULL) || (mesh->vertices == NULL)) return;

    const bool hasNormals = ((mesh->normals != NULL) && (mesh->animNormals != NULL));

    for (int v = 0; v < mesh->vertexCount; v++)
    {
        const Vector3 position = { mesh->vertices[v*3], mesh->vertices[v*3 + 1], mesh->vertices[v*3 + 2] };
        Vector3 normal = { 0.0f, 0.0f, 0.0f };
        if (hasNormals) normal = (Vector3){ mesh->normals[v*3], mesh->normals[v*3 + 1], mesh->normals[v*3 + 2] };

        Vector3 animPosition = { 0.0f, 0.0f, 0.0f };
        Vector3 animNormal = { 0.0f, 0.0f, 0.0f };
        float totalWeight = 0.0f;

        for (int j = 0; j < 4; j++)
        {
            const float weight = mesh->boneWeights[v*4 + j];
            if (weight == 0.0f) continue;

            const unsigned int boneId = mesh->boneIndices[v*4 + j];
            if (boneId >= boneCount) continue;

            Matrix boneMatrix = boneMatrices[boneId];

            animPosition = Vector3Add(animPosition, Vector3Scale(Vector3Transform(position, boneMatrix), weight));

            if (hasNormals)
            {
                // Normals ignore the translation component of the bone matrix
                boneMatrix.m12 = 0.0f;
                boneMatrix.m13 = 0.0f;
                boneMatrix.m14 = 0.0f;
                animNormal = Vector3Add(animNormal, Vector3Scale(Vector3Transform(normal, boneMatrix), weight));
            }

            totalWeight += weight;
        }

        if (totalWeight == 0.0f)
        {
            animPosition = position;
            animNormal = normal;
        }

        mesh->animVertices[v*3] = animPosition.x;
        mesh->animVertices[v*3 + 1] = animPosition.y;
        mesh->animVertices[v*3 + 2] = animPosition.z;

        if (hasNormals)
        {
            animNormal = Vector3Normalize(animNormal);
            mesh->animNormals[v*3] = animNormal.x;
            mesh->animNormals[v*3 + 1] = animNormal.y;
            mesh->animNormals[v*3 + 2] = animNormal.z;
        }
    }

    // Upload the animated vertex data to the GPU
    if (mesh->vboId != NULL)
    {
        if (mesh->vboId[MESH_VBO_POSITION] != 0)
        {
            rlUpdateVertexBuffer(mesh->vboId[MESH_VBO_POSITION], mesh->animVertices, mesh->vertexCount*3*(int)sizeof(float), 0);
        }

        if (hasNormals && (mesh->vboId[MESH_VBO_NORMAL] != 0))
        {
            rlUpdateVertexBuffer(mesh->vboId[MESH_VBO_NORMAL], mesh->animNormals, mesh->vertexCount*3*(int)sizeof(float), 0);
        }
    }
}

// Apply the model current pose to the bone palette and to every skinned mesh
static void ApplyModelPose(Model model)
{
    UpdateModelBoneMatrices(model);

    if (model.boneMatrices == NULL) return;

    for (int m = 0; m < model.meshCount; m++)
    {
        UpdateMeshSkinning(&model.meshes[m], model.boneMatrices, model.skeleton.boneCount);
    }
}

// Update model animated vertex data (positions and normals) for a given frame
// NOTE: Updated data is uploaded to GPU
void UpdateModelAnimation(Model model, ModelAnimation anim, float frame)
{
    if (!IsModelAnimationValid(model, anim)) return;
    if (model.currentPose == NULL) return;

    EvaluateAnimationPose(anim, frame, model.currentPose);
    ApplyModelPose(model);
}

// Update model animated vertex data (positions and normals) blending two animations
void UpdateModelAnimationEx(Model model, ModelAnimation animA, float frameA, ModelAnimation animB, float frameB, float blend)
{
    if (!IsModelAnimationValid(model, animA)) return;
    if (model.currentPose == NULL) return;

    if (!IsModelAnimationValid(model, animB))
    {
        UpdateModelAnimation(model, animA, frameA);
        return;
    }

    blend = Clamp(blend, 0.0f, 1.0f);

    Transform *poseB = (Transform *)RL_MALLOC((size_t)model.skeleton.boneCount*sizeof(Transform));
    if (poseB == NULL)
    {
        UpdateModelAnimation(model, animA, frameA);
        return;
    }

    EvaluateAnimationPose(animA, frameA, model.currentPose);
    EvaluateAnimationPose(animB, frameB, poseB);

    for (unsigned int i = 0; i < model.skeleton.boneCount; i++)
    {
        model.currentPose[i].translation = Vector3Lerp(model.currentPose[i].translation, poseB[i].translation, blend);
        model.currentPose[i].rotation = QuaternionSlerp(model.currentPose[i].rotation, poseB[i].rotation, blend);
        model.currentPose[i].scale = Vector3Lerp(model.currentPose[i].scale, poseB[i].scale, blend);
    }

    RL_FREE(poseB);

    ApplyModelPose(model);
}

// Unload animation array data
void UnloadModelAnimations(ModelAnimation *animations, int animCount)
{
    if (animations == NULL) return;

    for (int i = 0; i < animCount; i++)
    {
        if (animations[i].keyframePoses == NULL) continue;

        for (int j = 0; j < animations[i].keyframeCount; j++) RL_FREE(animations[i].keyframePoses[j]);

        RL_FREE(animations[i].keyframePoses);
    }

    RL_FREE(animations);
}

// Check model animation skeleton match
// NOTE: Only number of bones is checked, the API keeps no bone names on animations
bool IsModelAnimationValid(Model model, ModelAnimation anim)
{
    if (model.skeleton.boneCount == 0) return false;
    if (model.skeleton.boneCount != anim.boneCount) return false;
    if ((anim.keyframePoses == NULL) || (anim.keyframeCount <= 0)) return false;

    return true;
}

// Load model animations from file
ModelAnimation *LoadModelAnimations(const char *fileName, int *animCount)
{
    ModelAnimation *animations = NULL;
    int count = 0;

    if (fileName == NULL)
    {
        if (animCount != NULL) *animCount = 0;
        return NULL;
    }

#if defined(SUPPORT_FILEFORMAT_IQM)
    if (IsFileExtension(fileName, ".iqm")) animations = LoadModelAnimationsIQM(fileName, &count);
#endif
#if defined(SUPPORT_FILEFORMAT_M3D)
    if (IsFileExtension(fileName, ".m3d")) animations = LoadModelAnimationsM3D(fileName, &count);
#endif
#if defined(SUPPORT_FILEFORMAT_GLTF)
    if (IsFileExtension(fileName, ".gltf") || IsFileExtension(fileName, ".glb")) animations = LoadModelAnimationsGLTF(fileName, &count);
#endif

    if (animations == NULL) count = 0;

    if (animCount != NULL) *animCount = count;

    return animations;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Collision detection functions
//----------------------------------------------------------------------------------

// Check collision between two spheres
bool CheckCollisionSpheres(Vector3 center1, float radius1, Vector3 center2, float radius2)
{
    // Simple way to check for collision, just checking distance between two points
    // Compare squared distance to squared radius sum to avoid the square root
    const Vector3 d = Vector3Subtract(center2, center1);
    const float distanceSqr = d.x*d.x + d.y*d.y + d.z*d.z;
    const float radiusSum = radius1 + radius2;

    return (distanceSqr <= radiusSum*radiusSum);
}

// Check collision between two boxes
// NOTE: Boxes are defined by two points minimum and maximum
bool CheckCollisionBoxes(BoundingBox box1, BoundingBox box2)
{
    if ((box1.max.x < box2.min.x) || (box2.max.x < box1.min.x)) return false;
    if ((box1.max.y < box2.min.y) || (box2.max.y < box1.min.y)) return false;
    if ((box1.max.z < box2.min.z) || (box2.max.z < box1.min.z)) return false;

    return true;
}

// Check collision between box and sphere
bool CheckCollisionBoxSphere(BoundingBox box, Vector3 center, float radius)
{
    float dmin = 0.0f;

    if (center.x < box.min.x) dmin += (center.x - box.min.x)*(center.x - box.min.x);
    else if (center.x > box.max.x) dmin += (center.x - box.max.x)*(center.x - box.max.x);

    if (center.y < box.min.y) dmin += (center.y - box.min.y)*(center.y - box.min.y);
    else if (center.y > box.max.y) dmin += (center.y - box.max.y)*(center.y - box.max.y);

    if (center.z < box.min.z) dmin += (center.z - box.min.z)*(center.z - box.min.z);
    else if (center.z > box.max.z) dmin += (center.z - box.max.z)*(center.z - box.max.z);

    return (dmin <= (radius*radius));
}

// Get collision info between ray and sphere
RayCollision GetRayCollisionSphere(Ray ray, Vector3 center, float radius)
{
    RayCollision collision = { 0 };

    const Vector3 raySpherePos = Vector3Subtract(center, ray.position);
    const float vector = Vector3DotProduct(raySpherePos, ray.direction);
    const float distance = Vector3Length(raySpherePos);
    const float d = radius*radius - (distance*distance - vector*vector);

    collision.hit = (d >= 0.0f);
    if (!collision.hit) return collision;

    // Check if the ray origin is inside the sphere to calculate the correct collision point
    if (distance < radius) collision.distance = vector + sqrtf(d);
    else collision.distance = vector - sqrtf(d);

    collision.point = Vector3Add(ray.position, Vector3Scale(ray.direction, collision.distance));

    // Calculate collision normal (pointing outwards)
    collision.normal = Vector3Normalize(Vector3Subtract(collision.point, center));

    // Reverse the normal if the ray is inside the sphere
    if (distance < radius) collision.normal = Vector3Negate(collision.normal);

    return collision;
}

// Get collision info between ray and box
RayCollision GetRayCollisionBox(Ray ray, BoundingBox box)
{
    RayCollision collision = { 0 };

    // NOTE: If ray.position is inside the box, the distance is negative (as if the ray was reversed),
    // reversing ray.direction will give us the correct result
    const bool insideBox = (ray.position.x > box.min.x) && (ray.position.x < box.max.x) &&
                           (ray.position.y > box.min.y) && (ray.position.y < box.max.y) &&
                           (ray.position.z > box.min.z) && (ray.position.z < box.max.z);

    if (insideBox) ray.direction = Vector3Negate(ray.direction);

    const float invX = 1.0f/ray.direction.x;
    const float invY = 1.0f/ray.direction.y;
    const float invZ = 1.0f/ray.direction.z;

    const float t0 = (box.min.x - ray.position.x)*invX;
    const float t1 = (box.max.x - ray.position.x)*invX;
    const float t2 = (box.min.y - ray.position.y)*invY;
    const float t3 = (box.max.y - ray.position.y)*invY;
    const float t4 = (box.min.z - ray.position.z)*invZ;
    const float t5 = (box.max.z - ray.position.z)*invZ;

    const float tNear = fmaxf(fmaxf(fminf(t0, t1), fminf(t2, t3)), fminf(t4, t5));
    const float tFar = fminf(fminf(fmaxf(t0, t1), fmaxf(t2, t3)), fmaxf(t4, t5));

    collision.hit = !((tFar < 0.0f) || (tNear > tFar));
    collision.distance = tNear;
    collision.point = Vector3Add(ray.position, Vector3Scale(ray.direction, collision.distance));

    // Get the box center point, then the center->hit vector, then scale it to the unit cube:
    // the relevant components become slightly larger than 1.0 (or smaller than -1.0), so
    // truncating to integer and normalizing selects the closest axis
    collision.normal = Vector3Lerp(box.min, box.max, 0.5f);
    collision.normal = Vector3Subtract(collision.point, collision.normal);
    collision.normal = Vector3Scale(collision.normal, 2.0f);
    collision.normal = Vector3Divide(collision.normal, Vector3Subtract(box.max, box.min));

    collision.normal.x = (float)((int)collision.normal.x);
    collision.normal.y = (float)((int)collision.normal.y);
    collision.normal.z = (float)((int)collision.normal.z);

    collision.normal = Vector3Normalize(collision.normal);

    if (insideBox)
    {
        collision.distance *= -1.0f;
        collision.normal = Vector3Negate(collision.normal);
    }

    return collision;
}

// Get collision info between ray and triangle
// NOTE: The points are expected to be in counter-clockwise winding
// NOTE: Based on https://en.wikipedia.org/wiki/M%C3%B6ller%E2%80%93Trumbore_intersection_algorithm
RayCollision GetRayCollisionTriangle(Ray ray, Vector3 p1, Vector3 p2, Vector3 p3)
{
    RayCollision collision = { 0 };

    // Find vectors for two edges sharing V1
    const Vector3 edge1 = Vector3Subtract(p2, p1);
    const Vector3 edge2 = Vector3Subtract(p3, p1);

    // Begin calculating determinant - also used to calculate u parameter
    const Vector3 p = Vector3CrossProduct(ray.direction, edge2);

    // If determinant is near zero, ray lies in plane of triangle or ray is parallel to plane of triangle
    const float det = Vector3DotProduct(edge1, p);

    // Avoid culling! (backface culling would test for det < EPSILON)
    if ((det > -EPSILON) && (det < EPSILON)) return collision;

    const float invDet = 1.0f/det;

    // Calculate distance from V1 to ray origin
    const Vector3 tv = Vector3Subtract(ray.position, p1);

    // Calculate u parameter and test bound
    const float u = Vector3DotProduct(tv, p)*invDet;

    // The intersection lies outside the triangle
    if ((u < 0.0f) || (u > 1.0f)) return collision;

    // Prepare to test v parameter
    const Vector3 q = Vector3CrossProduct(tv, edge1);

    // Calculate V parameter and test bound
    const float v = Vector3DotProduct(ray.direction, q)*invDet;

    // The intersection lies outside the triangle
    if ((v < 0.0f) || ((u + v) > 1.0f)) return collision;

    const float t = Vector3DotProduct(edge2, q)*invDet;

    if (t > EPSILON)
    {
        // Ray hit, get hit point and normal
        collision.hit = true;
        collision.distance = t;
        collision.normal = Vector3Normalize(Vector3CrossProduct(edge1, edge2));
        collision.point = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    }

    return collision;
}

// Get collision info between ray and quad
// NOTE: The points are expected to be in counter-clockwise winding
RayCollision GetRayCollisionQuad(Ray ray, Vector3 p1, Vector3 p2, Vector3 p3, Vector3 p4)
{
    RayCollision collision = GetRayCollisionTriangle(ray, p1, p2, p4);

    if (!collision.hit) collision = GetRayCollisionTriangle(ray, p2, p3, p4);

    return collision;
}

// Get collision info between ray and mesh
RayCollision GetRayCollisionMesh(Ray ray, Mesh mesh, Matrix transform)
{
    RayCollision collision = { 0 };

    if ((mesh.vertices == NULL) || (mesh.triangleCount <= 0)) return collision;

    // Check if the mesh bounding box (transformed) is hit at all
    for (int i = 0; i < mesh.triangleCount; i++)
    {
        Vector3 a = { 0 };
        Vector3 b = { 0 };
        Vector3 c = { 0 };

        if (mesh.indices != NULL)
        {
            const int i0 = mesh.indices[i*3];
            const int i1 = mesh.indices[i*3 + 1];
            const int i2 = mesh.indices[i*3 + 2];

            a = (Vector3){ mesh.vertices[i0*3], mesh.vertices[i0*3 + 1], mesh.vertices[i0*3 + 2] };
            b = (Vector3){ mesh.vertices[i1*3], mesh.vertices[i1*3 + 1], mesh.vertices[i1*3 + 2] };
            c = (Vector3){ mesh.vertices[i2*3], mesh.vertices[i2*3 + 1], mesh.vertices[i2*3 + 2] };
        }
        else
        {
            a = (Vector3){ mesh.vertices[i*9], mesh.vertices[i*9 + 1], mesh.vertices[i*9 + 2] };
            b = (Vector3){ mesh.vertices[i*9 + 3], mesh.vertices[i*9 + 4], mesh.vertices[i*9 + 5] };
            c = (Vector3){ mesh.vertices[i*9 + 6], mesh.vertices[i*9 + 7], mesh.vertices[i*9 + 8] };
        }

        a = Vector3Transform(a, transform);
        b = Vector3Transform(b, transform);
        c = Vector3Transform(c, transform);

        const RayCollision triHitInfo = GetRayCollisionTriangle(ray, a, b, c);

        if (!triHitInfo.hit) continue;

        // Save the closest hit triangle
        if (!collision.hit || (collision.distance > triHitInfo.distance)) collision = triHitInfo;
    }

    return collision;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - File format loaders
//----------------------------------------------------------------------------------
#if defined(SUPPORT_FILEFORMAT_OBJ)
// Load OBJ mesh data
// NOTE: Vertex data is uploaded to GPU by the caller (LoadModel)
static Model LoadOBJ(const char *fileName)
{
    Model model = { 0 };
    model.transform = MatrixIdentity();

    char *fileText = LoadFileText(fileName);
    if (fileText == NULL)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] Unable to read obj file", fileName);
        return model;
    }

    // NOTE: tinyobj resolves the .mtl path relative to the current working directory
    char currentDir[1024] = { 0 };
    TextCopy(currentDir, GetWorkingDirectory());
    if (ChangeDirectory(GetDirectoryPath(fileName)) != 0)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] Failed to change working directory", fileName);
    }

    tinyobj_attrib_t attrib = { 0 };
    tinyobj_shape_t *shapes = NULL;
    unsigned int shapeCount = 0;
    tinyobj_material_t *objMaterials = NULL;
    unsigned int objMaterialCount = 0;

    const int result = tinyobj_parse_obj(&attrib, &shapes, &shapeCount, &objMaterials, &objMaterialCount,
                                         fileText, (unsigned int)strlen(fileText), TINYOBJ_FLAG_TRIANGULATE);

    UnloadFileText(fileText);

    if (result != TINYOBJ_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] Failed to load OBJ data", fileName);
        ChangeDirectory(currentDir);
        return model;
    }

    TRACELOG(LOG_INFO, "MODEL: [%s] OBJ data loaded successfully: %i meshes/%i materials", fileName, shapeCount, objMaterialCount);

    // The last material slot is the implicit default one, used by faces without material
    const int materialCount = (int)objMaterialCount + 1;
    // WARNING: tinyobj_loader_c naming is swapped vs what it suggests:
    // attrib.num_faces = face count (sizes material_ids/face_num_verts),
    // attrib.num_face_num_verts = total vertex-index count (sizes attrib.faces)
    const int faceCount = (int)attrib.num_faces;
    if (attrib.num_face_num_verts != attrib.num_faces*3)
    {
        // TINYOBJ_FLAG_TRIANGULATE guarantees 3 indices per face; anything else
        // would break the f*3 + k indexing below
        TRACELOG(LOG_WARNING, "MODEL: [%s] OBJ faces not fully triangulated, skipping", fileName);
        tinyobj_attrib_free(&attrib);
        tinyobj_shapes_free(shapes, shapeCount);
        tinyobj_materials_free(objMaterials, objMaterialCount);
        ChangeDirectory(currentDir);
        return model;
    }

    int *faceCountByMaterial = (int *)RL_CALLOC((size_t)materialCount, sizeof(int));
    int *meshIndexByMaterial = (int *)RL_CALLOC((size_t)materialCount, sizeof(int));

    if ((faceCountByMaterial == NULL) || (meshIndexByMaterial == NULL))
    {
        RL_FREE(faceCountByMaterial);
        RL_FREE(meshIndexByMaterial);
        tinyobj_attrib_free(&attrib);
        tinyobj_shapes_free(shapes, shapeCount);
        tinyobj_materials_free(objMaterials, objMaterialCount);
        ChangeDirectory(currentDir);
        return model;
    }

    for (int f = 0; f < faceCount; f++)
    {
        int matId = (attrib.material_ids != NULL)? attrib.material_ids[f] : -1;
        if ((matId < 0) || (matId >= (int)objMaterialCount)) matId = materialCount - 1;
        faceCountByMaterial[matId]++;
    }

    int meshCount = 0;
    for (int m = 0; m < materialCount; m++)
    {
        meshIndexByMaterial[m] = (faceCountByMaterial[m] > 0)? meshCount++ : -1;
    }

    model.meshCount = meshCount;
    model.meshes = (Mesh *)RL_CALLOC((size_t)((meshCount > 0)? meshCount : 1), sizeof(Mesh));
    model.meshMaterial = (int *)RL_CALLOC((size_t)((meshCount > 0)? meshCount : 1), sizeof(int));
    model.materialCount = materialCount;
    model.materials = (Material *)RL_CALLOC((size_t)materialCount, sizeof(Material));

    if ((model.meshes == NULL) || (model.meshMaterial == NULL) || (model.materials == NULL))
    {
        RL_FREE(model.meshes);
        RL_FREE(model.meshMaterial);
        RL_FREE(model.materials);
        model = (Model){ 0 };
        model.transform = MatrixIdentity();
    }
    else
    {
        // Allocate the per material meshes
        const bool hasTexcoords = (attrib.num_texcoords > 0);
        const bool hasNormals = (attrib.num_normals > 0);

        for (int m = 0; m < materialCount; m++)
        {
            const int meshIndex = meshIndexByMaterial[m];
            if (meshIndex < 0) continue;

            Mesh *mesh = &model.meshes[meshIndex];

            mesh->triangleCount = faceCountByMaterial[m];
            mesh->vertexCount = mesh->triangleCount*3;
            mesh->vertices = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
            if (hasTexcoords) mesh->texcoords = (float *)RL_CALLOC((size_t)mesh->vertexCount*2, sizeof(float));
            if (hasNormals) mesh->normals = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));

            model.meshMaterial[meshIndex] = m;

            // Reuse the per material counter as a write cursor
            faceCountByMaterial[m] = 0;
        }

        // Fill the vertex attributes, face by face
        for (int f = 0; f < faceCount; f++)
        {
            int matId = (attrib.material_ids != NULL)? attrib.material_ids[f] : -1;
            if ((matId < 0) || (matId >= (int)objMaterialCount)) matId = materialCount - 1;

            const int meshIndex = meshIndexByMaterial[matId];
            if (meshIndex < 0) continue;

            Mesh *mesh = &model.meshes[meshIndex];
            const int baseVertex = faceCountByMaterial[matId]*3;
            faceCountByMaterial[matId]++;

            for (int k = 0; k < 3; k++)
            {
                const tinyobj_vertex_index_t idx = attrib.faces[f*3 + k];
                const int v = baseVertex + k;

                if ((idx.v_idx >= 0) && ((unsigned int)idx.v_idx < attrib.num_vertices))
                {
                    mesh->vertices[v*3] = attrib.vertices[idx.v_idx*3];
                    mesh->vertices[v*3 + 1] = attrib.vertices[idx.v_idx*3 + 1];
                    mesh->vertices[v*3 + 2] = attrib.vertices[idx.v_idx*3 + 2];
                }

                if ((mesh->texcoords != NULL) && (idx.vt_idx >= 0) && ((unsigned int)idx.vt_idx < attrib.num_texcoords))
                {
                    mesh->texcoords[v*2] = attrib.texcoords[idx.vt_idx*2];
                    // NOTE: OBJ texcoord origin is bottom-left, raylib expects top-left
                    mesh->texcoords[v*2 + 1] = 1.0f - attrib.texcoords[idx.vt_idx*2 + 1];
                }

                if ((mesh->normals != NULL) && (idx.vn_idx >= 0) && ((unsigned int)idx.vn_idx < attrib.num_normals))
                {
                    mesh->normals[v*3] = attrib.normals[idx.vn_idx*3];
                    mesh->normals[v*3 + 1] = attrib.normals[idx.vn_idx*3 + 1];
                    mesh->normals[v*3 + 2] = attrib.normals[idx.vn_idx*3 + 2];
                }
            }
        }

        // Load the materials (the last slot is the implicit default material)
        if (objMaterialCount > 0) ProcessMaterialsOBJ(model.materials, objMaterials, (int)objMaterialCount, ".");
        model.materials[materialCount - 1] = LoadMaterialDefault();
    }

    RL_FREE(faceCountByMaterial);
    RL_FREE(meshIndexByMaterial);

    tinyobj_attrib_free(&attrib);
    tinyobj_shapes_free(shapes, shapeCount);
    tinyobj_materials_free(objMaterials, objMaterialCount);

    ChangeDirectory(currentDir);

    return model;
}
#else
static Model LoadOBJ(const char *fileName) { (void)fileName; return (Model){ 0 }; }
#endif

#if defined(SUPPORT_FILEFORMAT_IQM)
//----------------------------------------------------------------------------------
// Inter-Quake Model (IQM) binary format
// NOTE: There is no vendored IQM header, the (simple) chunked format is parsed inline
//----------------------------------------------------------------------------------
#define IQM_MAGIC       "INTERQUAKEMODEL"
#define IQM_VERSION     2

typedef struct IQMHeader {
    char magic[16];
    unsigned int version;
    unsigned int dataSize;
    unsigned int flags;
    unsigned int num_text, ofs_text;
    unsigned int num_meshes, ofs_meshes;
    unsigned int num_vertexarrays, num_vertexes, ofs_vertexarrays;
    unsigned int num_triangles, ofs_triangles, ofs_adjacency;
    unsigned int num_joints, ofs_joints;
    unsigned int num_poses, ofs_poses;
    unsigned int num_anims, ofs_anims;
    unsigned int num_frames, num_framechannels, ofs_frames, ofs_bounds;
    unsigned int num_comment, ofs_comment;
    unsigned int num_extensions, ofs_extensions;
} IQMHeader;

typedef struct IQMMesh {
    unsigned int name;
    unsigned int material;
    unsigned int first_vertex, num_vertexes;
    unsigned int first_triangle, num_triangles;
} IQMMesh;

typedef struct IQMTriangle {
    unsigned int vertex[3];
} IQMTriangle;

typedef struct IQMJoint {
    unsigned int name;
    int parent;
    float translate[3], rotate[4], scale[3];
} IQMJoint;

typedef struct IQMPose {
    int parent;
    unsigned int mask;
    float channeloffset[10];
    float channelscale[10];
} IQMPose;

typedef struct IQMAnim {
    unsigned int name;
    unsigned int first_frame, num_frames;
    float framerate;
    unsigned int flags;
} IQMAnim;

typedef struct IQMVertexArray {
    unsigned int type;
    unsigned int flags;
    unsigned int format;
    unsigned int size;
    unsigned int offset;
} IQMVertexArray;

// IQM vertex array types
enum {
    IQM_POSITION = 0,
    IQM_TEXCOORD = 1,
    IQM_NORMAL = 2,
    IQM_TANGENT = 3,        // NOTE: Tangents unused by default
    IQM_BLENDINDEXES = 4,
    IQM_BLENDWEIGHTS = 5,
    IQM_COLOR = 6,
    IQM_CUSTOM = 0x10       // NOTE: Custom vertex values unused by default
};

// Read the IQM header and validate it against the loaded file data
// NOTE: Returns false (and logs) on any inconsistency; header is left zeroed then
static bool ReadIQMHeader(const unsigned char *fileData, int dataSize, IQMHeader *header, const char *fileName)
{
    memset(header, 0, sizeof(IQMHeader));

    if ((fileData == NULL) || (dataSize < (int)sizeof(IQMHeader))) return false;

    memcpy(header, fileData, sizeof(IQMHeader));

    if (memcmp(header->magic, IQM_MAGIC, sizeof(IQM_MAGIC)) != 0)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] IQM file is not a valid model", fileName);
        return false;
    }

    if (header->version != IQM_VERSION)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] IQM file version not supported (%i)", fileName, header->version);
        return false;
    }

    if (header->dataSize > (unsigned int)dataSize)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] IQM file is truncated", fileName);
        return false;
    }

    return true;
}

// Check that a chunk of 'count' elements of 'size' bytes fits inside the file data
static bool IsIQMChunkValid(unsigned int offset, unsigned int count, size_t size, int dataSize)
{
    if (count == 0) return false;
    if (offset > (unsigned int)dataSize) return false;

    return (((size_t)count*size) <= (size_t)(dataSize - (int)offset));
}

// Copy a NUL terminated bone name out of the IQM text chunk
static void CopyIQMName(char *dst, size_t dstSize, const unsigned char *fileData, int dataSize,
                        unsigned int textOffset, unsigned int nameOffset)
{
    memset(dst, 0, dstSize);

    if (textOffset == 0) return;
    if ((size_t)textOffset + nameOffset >= (size_t)dataSize) return;

    const char *src = (const char *)(fileData + textOffset + nameOffset);
    const size_t maxLen = (size_t)dataSize - (textOffset + nameOffset);

    size_t i = 0;
    while ((i < (dstSize - 1)) && (i < maxLen) && (src[i] != '\0'))
    {
        dst[i] = src[i];
        i++;
    }
}

// Load IQM mesh data
static Model LoadIQM(const char *fileName)
{
    Model model = { 0 };
    model.transform = MatrixIdentity();

    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);
    if (fileData == NULL) return model;

    IQMHeader header = { 0 };
    if (!ReadIQMHeader(fileData, dataSize, &header, fileName))
    {
        UnloadFileData(fileData);
        return model;
    }

    if (!IsIQMChunkValid(header.ofs_meshes, header.num_meshes, sizeof(IQMMesh), dataSize) ||
        !IsIQMChunkValid(header.ofs_triangles, header.num_triangles, sizeof(IQMTriangle), dataSize) ||
        !IsIQMChunkValid(header.ofs_vertexarrays, header.num_vertexarrays, sizeof(IQMVertexArray), dataSize))
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] IQM chunks out of file bounds", fileName);
        UnloadFileData(fileData);
        return model;
    }

    IQMMesh *imesh = (IQMMesh *)RL_MALLOC((size_t)header.num_meshes*sizeof(IQMMesh));
    IQMTriangle *tri = (IQMTriangle *)RL_MALLOC((size_t)header.num_triangles*sizeof(IQMTriangle));
    IQMVertexArray *va = (IQMVertexArray *)RL_MALLOC((size_t)header.num_vertexarrays*sizeof(IQMVertexArray));

    if ((imesh == NULL) || (tri == NULL) || (va == NULL))
    {
        RL_FREE(imesh);
        RL_FREE(tri);
        RL_FREE(va);
        UnloadFileData(fileData);
        return model;
    }

    memcpy(imesh, fileData + header.ofs_meshes, (size_t)header.num_meshes*sizeof(IQMMesh));
    memcpy(tri, fileData + header.ofs_triangles, (size_t)header.num_triangles*sizeof(IQMTriangle));
    memcpy(va, fileData + header.ofs_vertexarrays, (size_t)header.num_vertexarrays*sizeof(IQMVertexArray));

    // Gather the global vertex attribute arrays
    float *vertex = NULL;
    float *normal = NULL;
    float *text = NULL;
    unsigned char *blendi = NULL;
    float *blendw = NULL;
    unsigned char *color = NULL;

    for (unsigned int i = 0; i < header.num_vertexarrays; i++)
    {
        const size_t floatCount = (size_t)header.num_vertexes*va[i].size;

        switch (va[i].type)
        {
            case IQM_POSITION:
            {
                if (va[i].size != 3) break;
                vertex = (float *)RL_MALLOC(floatCount*sizeof(float));
                if (vertex != NULL) memcpy(vertex, fileData + va[i].offset, floatCount*sizeof(float));
            } break;
            case IQM_NORMAL:
            {
                if (va[i].size != 3) break;
                normal = (float *)RL_MALLOC(floatCount*sizeof(float));
                if (normal != NULL) memcpy(normal, fileData + va[i].offset, floatCount*sizeof(float));
            } break;
            case IQM_TEXCOORD:
            {
                if (va[i].size != 2) break;
                text = (float *)RL_MALLOC(floatCount*sizeof(float));
                if (text != NULL) memcpy(text, fileData + va[i].offset, floatCount*sizeof(float));
            } break;
            case IQM_BLENDINDEXES:
            {
                if (va[i].size != 4) break;
                blendi = (unsigned char *)RL_MALLOC(floatCount*sizeof(unsigned char));
                if (blendi != NULL) memcpy(blendi, fileData + va[i].offset, floatCount*sizeof(unsigned char));
            } break;
            case IQM_BLENDWEIGHTS:
            {
                if (va[i].size != 4) break;
                blendw = (float *)RL_MALLOC(floatCount*sizeof(float));
                if (blendw == NULL) break;
                // format: 1 = IQM_UBYTE (normalized, the common case e.g. guy.iqm), 7 = IQM_FLOAT
                if (va[i].format == 1)
                {
                    const unsigned char *raw = fileData + va[i].offset;
                    for (unsigned int w = 0; w < floatCount; w++) blendw[w] = (float)raw[w]/255.0f;
                }
                else memcpy(blendw, fileData + va[i].offset, floatCount*sizeof(float));
            } break;
            case IQM_COLOR:
            {
                if (va[i].size != 4) break;
                color = (unsigned char *)RL_MALLOC(floatCount*sizeof(unsigned char));
                if (color != NULL) memcpy(color, fileData + va[i].offset, floatCount*sizeof(unsigned char));
            } break;
            default: break;
        }
    }

    model.meshCount = (int)header.num_meshes;
    model.meshes = (Mesh *)RL_CALLOC((size_t)model.meshCount, sizeof(Mesh));
    model.meshMaterial = (int *)RL_CALLOC((size_t)model.meshCount, sizeof(int));
    model.materialCount = 1;
    model.materials = (Material *)RL_CALLOC(1, sizeof(Material));
    if (model.materials != NULL) model.materials[0] = LoadMaterialDefault();

    for (int i = 0; (model.meshes != NULL) && (i < model.meshCount); i++)
    {
        Mesh *mesh = &model.meshes[i];

        mesh->vertexCount = (int)imesh[i].num_vertexes;
        mesh->triangleCount = (int)imesh[i].num_triangles;

        const size_t first = imesh[i].first_vertex;

        if (vertex != NULL)
        {
            mesh->vertices = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
            if (mesh->vertices != NULL) memcpy(mesh->vertices, vertex + first*3, (size_t)mesh->vertexCount*3*sizeof(float));

            mesh->animVertices = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
            if ((mesh->animVertices != NULL) && (mesh->vertices != NULL))
            {
                memcpy(mesh->animVertices, mesh->vertices, (size_t)mesh->vertexCount*3*sizeof(float));
            }
        }

        if (normal != NULL)
        {
            mesh->normals = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
            if (mesh->normals != NULL) memcpy(mesh->normals, normal + first*3, (size_t)mesh->vertexCount*3*sizeof(float));

            mesh->animNormals = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
            if ((mesh->animNormals != NULL) && (mesh->normals != NULL))
            {
                memcpy(mesh->animNormals, mesh->normals, (size_t)mesh->vertexCount*3*sizeof(float));
            }
        }

        if (text != NULL)
        {
            mesh->texcoords = (float *)RL_CALLOC((size_t)mesh->vertexCount*2, sizeof(float));
            if (mesh->texcoords != NULL) memcpy(mesh->texcoords, text + first*2, (size_t)mesh->vertexCount*2*sizeof(float));
        }

        if (color != NULL)
        {
            mesh->colors = (unsigned char *)RL_CALLOC((size_t)mesh->vertexCount*4, sizeof(unsigned char));
            if (mesh->colors != NULL) memcpy(mesh->colors, color + first*4, (size_t)mesh->vertexCount*4*sizeof(unsigned char));
        }

        if (blendi != NULL)
        {
            mesh->boneIndices = (unsigned char *)RL_CALLOC((size_t)mesh->vertexCount*4, sizeof(unsigned char));
            if (mesh->boneIndices != NULL) memcpy(mesh->boneIndices, blendi + first*4, (size_t)mesh->vertexCount*4*sizeof(unsigned char));
        }

        if (blendw != NULL)
        {
            mesh->boneWeights = (float *)RL_CALLOC((size_t)mesh->vertexCount*4, sizeof(float));
            if (mesh->boneWeights != NULL) memcpy(mesh->boneWeights, blendw + first*4, (size_t)mesh->vertexCount*4*sizeof(float));
        }

        mesh->boneCount = (int)header.num_joints;

        mesh->indices = (unsigned short *)RL_CALLOC((size_t)mesh->triangleCount*3, sizeof(unsigned short));
        if (mesh->indices != NULL)
        {
            for (int t = 0; t < mesh->triangleCount; t++)
            {
                // NOTE: IQM triangle indices are counter-clockwise, raylib expects the reversed order
                const IQMTriangle *src = &tri[t + imesh[i].first_triangle];
                mesh->indices[t*3 + 2] = (unsigned short)(src->vertex[0] - imesh[i].first_vertex);
                mesh->indices[t*3 + 1] = (unsigned short)(src->vertex[1] - imesh[i].first_vertex);
                mesh->indices[t*3] = (unsigned short)(src->vertex[2] - imesh[i].first_vertex);
            }
        }

        model.meshMaterial[i] = 0;
    }

    // Load the skeleton (bones hierarchy and bind pose)
    if (IsIQMChunkValid(header.ofs_joints, header.num_joints, sizeof(IQMJoint), dataSize))
    {
        IQMJoint *joints = (IQMJoint *)RL_MALLOC((size_t)header.num_joints*sizeof(IQMJoint));

        if (joints != NULL)
        {
            memcpy(joints, fileData + header.ofs_joints, (size_t)header.num_joints*sizeof(IQMJoint));

            model.skeleton.boneCount = header.num_joints;
            model.skeleton.bones = (BoneInfo *)RL_CALLOC((size_t)header.num_joints, sizeof(BoneInfo));
            model.skeleton.bindPose = (Transform *)RL_CALLOC((size_t)header.num_joints, sizeof(Transform));

            if ((model.skeleton.bones != NULL) && (model.skeleton.bindPose != NULL))
            {
                for (unsigned int i = 0; i < header.num_joints; i++)
                {
                    CopyIQMName(model.skeleton.bones[i].name, sizeof(model.skeleton.bones[i].name),
                                fileData, dataSize, header.ofs_text, joints[i].name);
                    model.skeleton.bones[i].parent = joints[i].parent;

                    model.skeleton.bindPose[i].translation = (Vector3){ joints[i].translate[0], joints[i].translate[1], joints[i].translate[2] };
                    model.skeleton.bindPose[i].rotation = QuaternionNormalize((Quaternion){ joints[i].rotate[0], joints[i].rotate[1], joints[i].rotate[2], joints[i].rotate[3] });
                    model.skeleton.bindPose[i].scale = (Vector3){ joints[i].scale[0], joints[i].scale[1], joints[i].scale[2] };
                }

                BuildPoseFromParentJoints(model.skeleton.bones, model.skeleton.boneCount, model.skeleton.bindPose);
            }
            else
            {
                RL_FREE(model.skeleton.bones);
                RL_FREE(model.skeleton.bindPose);
                model.skeleton = (ModelSkeleton){ 0 };
            }

            RL_FREE(joints);
        }
    }

    RL_FREE(imesh);
    RL_FREE(tri);
    RL_FREE(va);
    RL_FREE(vertex);
    RL_FREE(normal);
    RL_FREE(text);
    RL_FREE(blendi);
    RL_FREE(blendw);
    RL_FREE(color);

    UnloadFileData(fileData);

    return model;
}

// Load IQM animation data
static ModelAnimation *LoadModelAnimationsIQM(const char *fileName, int *animCount)
{
    if (animCount != NULL) *animCount = 0;

    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);
    if (fileData == NULL) return NULL;

    IQMHeader header = { 0 };
    if (!ReadIQMHeader(fileData, dataSize, &header, fileName))
    {
        UnloadFileData(fileData);
        return NULL;
    }

    if (!IsIQMChunkValid(header.ofs_anims, header.num_anims, sizeof(IQMAnim), dataSize) ||
        !IsIQMChunkValid(header.ofs_poses, header.num_poses, sizeof(IQMPose), dataSize) ||
        !IsIQMChunkValid(header.ofs_frames, (unsigned int)((size_t)header.num_frames*header.num_framechannels), sizeof(unsigned short), dataSize))
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] IQM file does not contain valid animation data", fileName);
        UnloadFileData(fileData);
        return NULL;
    }

    IQMAnim *anim = (IQMAnim *)RL_MALLOC((size_t)header.num_anims*sizeof(IQMAnim));
    IQMPose *poses = (IQMPose *)RL_MALLOC((size_t)header.num_poses*sizeof(IQMPose));
    unsigned short *framedata = (unsigned short *)RL_MALLOC((size_t)header.num_frames*header.num_framechannels*sizeof(unsigned short));
    ModelAnimation *animations = (ModelAnimation *)RL_CALLOC((size_t)header.num_anims, sizeof(ModelAnimation));
    BoneInfo *bones = (BoneInfo *)RL_CALLOC((size_t)header.num_poses, sizeof(BoneInfo));

    if ((anim == NULL) || (poses == NULL) || (framedata == NULL) || (animations == NULL) || (bones == NULL))
    {
        RL_FREE(anim);
        RL_FREE(poses);
        RL_FREE(framedata);
        RL_FREE(animations);
        RL_FREE(bones);
        UnloadFileData(fileData);
        return NULL;
    }

    memcpy(anim, fileData + header.ofs_anims, (size_t)header.num_anims*sizeof(IQMAnim));
    memcpy(poses, fileData + header.ofs_poses, (size_t)header.num_poses*sizeof(IQMPose));
    memcpy(framedata, fileData + header.ofs_frames, (size_t)header.num_frames*header.num_framechannels*sizeof(unsigned short));

    // The pose hierarchy is needed to turn parent-relative frames into model space poses
    for (unsigned int i = 0; i < header.num_poses; i++) bones[i].parent = poses[i].parent;

    for (unsigned int a = 0; a < header.num_anims; a++)
    {
        animations[a].boneCount = header.num_poses;
        animations[a].keyframeCount = (int)anim[a].num_frames;
        animations[a].keyframePoses = (Transform **)RL_CALLOC((size_t)anim[a].num_frames, sizeof(Transform *));

        CopyIQMName(animations[a].name, sizeof(animations[a].name), fileData, dataSize, header.ofs_text, anim[a].name);

        if (animations[a].keyframePoses == NULL)
        {
            animations[a].keyframeCount = 0;
            continue;
        }

        for (unsigned int frame = 0; frame < anim[a].num_frames; frame++)
        {
            animations[a].keyframePoses[frame] = (Transform *)RL_CALLOC((size_t)header.num_poses, sizeof(Transform));
            if (animations[a].keyframePoses[frame] == NULL) continue;

            size_t dcounter = (size_t)(anim[a].first_frame + frame)*header.num_framechannels;

            for (unsigned int i = 0; i < header.num_poses; i++)
            {
                float channel[10];

                for (int c = 0; c < 10; c++)
                {
                    channel[c] = poses[i].channeloffset[c];

                    if (poses[i].mask & (1u << c))
                    {
                        channel[c] += (float)framedata[dcounter]*poses[i].channelscale[c];
                        dcounter++;
                    }
                }

                Transform t = { 0 };
                t.translation = (Vector3){ channel[0], channel[1], channel[2] };
                t.rotation = QuaternionNormalize((Quaternion){ channel[3], channel[4], channel[5], channel[6] });
                t.scale = (Vector3){ channel[7], channel[8], channel[9] };

                animations[a].keyframePoses[frame][i] = t;
            }

            // Build the absolute (model space) pose out of the parent-relative frame
            BuildPoseFromParentJoints(bones, header.num_poses, animations[a].keyframePoses[frame]);
        }
    }

    RL_FREE(anim);
    RL_FREE(poses);
    RL_FREE(framedata);
    RL_FREE(bones);

    UnloadFileData(fileData);

    if (animCount != NULL) *animCount = (int)header.num_anims;

    return animations;
}
#else
static Model LoadIQM(const char *fileName) { (void)fileName; return (Model){ 0 }; }
static ModelAnimation *LoadModelAnimationsIQM(const char *fileName, int *animCount) { (void)fileName; if (animCount != NULL) *animCount = 0; return NULL; }
#endif

#if defined(SUPPORT_FILEFORMAT_VOX)
// Load MagicaVoxel (.vox) model data
static Model LoadVOX(const char *fileName)
{
    Model model = { 0 };
    model.transform = MatrixIdentity();

    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);
    if (fileData == NULL)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] Failed to load VOX file", fileName);
        return model;
    }

    VoxArray3D voxarray = { 0 };
    const int result = Vox_LoadFromMemory(fileData, (unsigned int)dataSize, &voxarray);

    UnloadFileData(fileData);

    if (result != VOX_SUCCESS)
    {
        Vox_FreeArrays(&voxarray);
        TRACELOG(LOG_WARNING, "MODEL: [%s] Failed to load VOX data", fileName);
        return model;
    }

    // NOTE: Meshes are split so that vertex indices always fit an unsigned short
    // 6*4 = 24 vertices per voxel, 5461 voxels x 12 vertices -> 65532 (must be < 65536)
    const int verticesMax = 65532;
    const int verticesTotal = voxarray.vertices.used;

    if (verticesTotal <= 0)
    {
        Vox_FreeArrays(&voxarray);
        TRACELOG(LOG_WARNING, "MODEL: [%s] VOX data contains no geometry", fileName);
        return model;
    }

    model.meshCount = 1 + (verticesTotal - 1)/verticesMax;
    model.meshes = (Mesh *)RL_CALLOC((size_t)model.meshCount, sizeof(Mesh));
    model.meshMaterial = (int *)RL_CALLOC((size_t)model.meshCount, sizeof(int));
    model.materialCount = 1;
    model.materials = (Material *)RL_CALLOC(1, sizeof(Material));

    if ((model.meshes == NULL) || (model.meshMaterial == NULL) || (model.materials == NULL))
    {
        RL_FREE(model.meshes);
        RL_FREE(model.meshMaterial);
        RL_FREE(model.materials);
        Vox_FreeArrays(&voxarray);
        return (Model){ 0 };
    }

    model.materials[0] = LoadMaterialDefault();

    const VoxVector3 *srcVertices = voxarray.vertices.array;
    const VoxVector3 *srcNormals = voxarray.normals.array;
    const VoxColor *srcColors = voxarray.colors.array;

    int verticesRemain = verticesTotal;

    for (int m = 0; m < model.meshCount; m++)
    {
        Mesh *mesh = &model.meshes[m];

        mesh->vertexCount = (verticesRemain < verticesMax)? verticesRemain : verticesMax;
        mesh->triangleCount = (mesh->vertexCount/4)*2;

        mesh->vertices = (float *)RL_MALLOC((size_t)mesh->vertexCount*3*sizeof(float));
        mesh->normals = (float *)RL_MALLOC((size_t)mesh->vertexCount*3*sizeof(float));
        mesh->colors = (unsigned char *)RL_MALLOC((size_t)mesh->vertexCount*4*sizeof(unsigned char));
        mesh->indices = (unsigned short *)RL_MALLOC((size_t)mesh->triangleCount*3*sizeof(unsigned short));

        if ((mesh->vertices == NULL) || (mesh->normals == NULL) || (mesh->colors == NULL) || (mesh->indices == NULL))
        {
            TRACELOG(LOG_WARNING, "MODEL: [%s] Failed to allocate VOX mesh data", fileName);
            break;
        }

        memcpy(mesh->vertices, srcVertices, (size_t)mesh->vertexCount*3*sizeof(float));
        memcpy(mesh->normals, srcNormals, (size_t)mesh->vertexCount*3*sizeof(float));
        memcpy(mesh->colors, srcColors, (size_t)mesh->vertexCount*4*sizeof(unsigned char));

        // Rebuild the quad indices, matching the vox_loader winding (v0-v2-v1, v0-v3-v2)
        for (int q = 0; q < mesh->vertexCount/4; q++)
        {
            const unsigned short base = (unsigned short)(q*4);

            mesh->indices[q*6] = base;
            mesh->indices[q*6 + 1] = (unsigned short)(base + 2);
            mesh->indices[q*6 + 2] = (unsigned short)(base + 1);
            mesh->indices[q*6 + 3] = base;
            mesh->indices[q*6 + 4] = (unsigned short)(base + 3);
            mesh->indices[q*6 + 5] = (unsigned short)(base + 2);
        }

        model.meshMaterial[m] = 0;

        verticesRemain -= mesh->vertexCount;
        srcVertices += mesh->vertexCount;
        srcNormals += mesh->vertexCount;
        srcColors += mesh->vertexCount;
    }

    Vox_FreeArrays(&voxarray);

    return model;
}
#else
static Model LoadVOX(const char *fileName) { (void)fileName; return (Model){ 0 }; }
#endif

#if defined(SUPPORT_FILEFORMAT_M3D)
#define M3D_ANIMDELAY   17      // Animation frame delay in ms (roughly 60 fps)

// Get a raylib Color out of an M3D 0xAABBGGRR packed color value
static Color ColorFromM3D(uint32_t value)
{
    const Color color = {
        (unsigned char)(value & 0xff),
        (unsigned char)((value >> 8) & 0xff),
        (unsigned char)((value >> 16) & 0xff),
        (unsigned char)((value >> 24) & 0xff)
    };

    return color;
}

// Load an M3D inlined texture into a raylib texture
static Texture2D LoadTextureFromM3D(const m3dtx_t *tx)
{
    Texture2D texture = { 0 };

    if ((tx == NULL) || (tx->d == NULL) || (tx->w == 0) || (tx->h == 0)) return texture;

    int format = 0;
    switch (tx->f)
    {
        case 1: format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE; break;
        case 2: format = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA; break;
        case 3: format = PIXELFORMAT_UNCOMPRESSED_R8G8B8; break;
        case 4: format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8; break;
        default: return texture;
    }

    const Image image = { tx->d, (int)tx->w, (int)tx->h, 1, format };

    return LoadTextureFromImage(image);
}

// Convert a parent-relative M3D transform into model space
static void M3DComposeWithParent(Transform *transforms, const BoneInfo *bones, int index)
{
    const int parent = bones[index].parent;
    if (parent < 0) return;

    transforms[index].rotation = QuaternionMultiply(transforms[parent].rotation, transforms[index].rotation);
    transforms[index].translation = Vector3RotateByQuaternion(transforms[index].translation, transforms[parent].rotation);
    transforms[index].translation = Vector3Add(transforms[index].translation, transforms[parent].translation);
    transforms[index].scale = Vector3Multiply(transforms[index].scale, transforms[parent].scale);
}

// Load the material array of an M3D model
static void LoadMaterialsM3D(Model *model, const m3d_t *m3d)
{
    // raylib convention: materials[0] stays the default material, M3D materials at 1..N
    for (int i = 0; i < model->materialCount; i++) model->materials[i] = LoadMaterialDefault();

    for (unsigned int i = 0; i < m3d->nummaterial; i++)
    {
        Material *material = &model->materials[i + 1];
        if (material->maps == NULL) continue;

        const m3dm_t *src = &m3d->material[i];

        for (unsigned int j = 0; j < src->numprop; j++)
        {
            const m3dp_t *prop = &src->prop[j];

            switch (prop->type)
            {
                case m3dp_Kd: material->maps[MATERIAL_MAP_DIFFUSE].color = ColorFromM3D(prop->value.color); break;
                case m3dp_Ks: material->maps[MATERIAL_MAP_SPECULAR].color = ColorFromM3D(prop->value.color); break;
                case m3dp_Ns: material->maps[MATERIAL_MAP_SPECULAR].value = prop->value.fnum; break;
                case m3dp_Ke: material->maps[MATERIAL_MAP_EMISSION].color = ColorFromM3D(prop->value.color); break;
                case m3dp_Pm: material->maps[MATERIAL_MAP_METALNESS].value = prop->value.fnum; break;
                case m3dp_Pr: material->maps[MATERIAL_MAP_ROUGHNESS].value = prop->value.fnum; break;
                case m3dp_map_Kd:
                case m3dp_map_Ks:
                case m3dp_map_Ke:
                case m3dp_map_N:
                case m3dp_map_Pm:
                case m3dp_map_Pr:
                {
                    if (prop->value.textureid >= m3d->numtexture) break;

                    const Texture2D texture = LoadTextureFromM3D(&m3d->texture[prop->value.textureid]);
                    if (texture.id == 0) break;

                    int mapType = MATERIAL_MAP_DIFFUSE;
                    if (prop->type == m3dp_map_Ks) mapType = MATERIAL_MAP_SPECULAR;
                    else if (prop->type == m3dp_map_Ke) mapType = MATERIAL_MAP_EMISSION;
                    else if (prop->type == m3dp_map_N) mapType = MATERIAL_MAP_NORMAL;
                    else if (prop->type == m3dp_map_Pm) mapType = MATERIAL_MAP_METALNESS;
                    else if (prop->type == m3dp_map_Pr) mapType = MATERIAL_MAP_ROUGHNESS;

                    material->maps[mapType].texture = texture;
                } break;
                default: break;
            }
        }
    }
}

// Load the skeleton (bones and bind pose) of an M3D model
// NOTE: An extra "no bone" entry is appended, used by vertices without skin data
static void LoadSkeletonM3D(Model *model, const m3d_t *m3d)
{
    if (m3d->numbone == 0) return;

    const unsigned int boneCount = m3d->numbone + 1;

    model->skeleton.bones = (BoneInfo *)RL_CALLOC(boneCount, sizeof(BoneInfo));
    model->skeleton.bindPose = (Transform *)RL_CALLOC(boneCount, sizeof(Transform));

    if ((model->skeleton.bones == NULL) || (model->skeleton.bindPose == NULL))
    {
        RL_FREE(model->skeleton.bones);
        RL_FREE(model->skeleton.bindPose);
        model->skeleton = (ModelSkeleton){ 0 };
        return;
    }

    model->skeleton.boneCount = boneCount;

    for (unsigned int i = 0; i < m3d->numbone; i++)
    {
        model->skeleton.bones[i].parent = (int)m3d->bone[i].parent;
        if (m3d->bone[i].parent == M3D_UNDEF) model->skeleton.bones[i].parent = -1;

        if (m3d->bone[i].name != NULL)
        {
            strncpy(model->skeleton.bones[i].name, m3d->bone[i].name, sizeof(model->skeleton.bones[i].name) - 1);
        }

        const m3dv_t *pos = &m3d->vertex[m3d->bone[i].pos];
        const m3dv_t *ori = &m3d->vertex[m3d->bone[i].ori];

        model->skeleton.bindPose[i].translation = (Vector3){ (float)pos->x*m3d->scale, (float)pos->y*m3d->scale, (float)pos->z*m3d->scale };
        model->skeleton.bindPose[i].rotation = QuaternionNormalize((Quaternion){ (float)ori->x, (float)ori->y, (float)ori->z, (float)ori->w });
        model->skeleton.bindPose[i].scale = (Vector3){ 1.0f, 1.0f, 1.0f };

        // Child bones are stored in parent bone relative space, convert that into model space
        M3DComposeWithParent(model->skeleton.bindPose, model->skeleton.bones, (int)i);
    }

    // Add the special "no bone" bone
    model->skeleton.bones[m3d->numbone].parent = -1;
    TextCopy(model->skeleton.bones[m3d->numbone].name, "NO BONE");
    model->skeleton.bindPose[m3d->numbone].translation = (Vector3){ 0.0f, 0.0f, 0.0f };
    model->skeleton.bindPose[m3d->numbone].rotation = QuaternionIdentity();
    model->skeleton.bindPose[m3d->numbone].scale = (Vector3){ 1.0f, 1.0f, 1.0f };
}

// Load M3D mesh data
static Model LoadM3D(const char *fileName)
{
    Model model = { 0 };
    model.transform = MatrixIdentity();

    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);
    if (fileData == NULL) return model;

    m3d_t *m3d = m3d_load(fileData, NULL, NULL, NULL);

    if ((m3d == NULL) || M3D_ERR_ISFATAL(m3d->errcode))
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] M3D data could not be loaded", fileName);
        if (m3d != NULL) m3d_free(m3d);
        UnloadFileData(fileData);
        return model;
    }

    TRACELOG(LOG_INFO, "MODEL: [%s] M3D data loaded successfully: %i faces/%i materials",
             fileName, m3d->numface, m3d->nummaterial);

    if (m3d->numface == 0)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] M3D data contains no faces", fileName);
        m3d_free(m3d);
        UnloadFileData(fileData);
        return model;
    }

    // NOTE: Faces are grouped by material in the file, one raylib mesh per group
    int meshCount = 0;
    M3D_INDEX lastMaterial = M3D_UNDEF - 1;
    for (unsigned int i = 0; i < m3d->numface; i++)
    {
        if (m3d->face[i].materialid != lastMaterial)
        {
            lastMaterial = m3d->face[i].materialid;
            meshCount++;
        }
    }

    const bool hasSkin = ((m3d->numbone > 0) && (m3d->numskin > 0));
    bool hasVertexColors = false;
    for (unsigned int i = 0; i < m3d->numvertex; i++)
    {
        if (m3d->vertex[i].color != 0) { hasVertexColors = true; break; }
    }

    model.meshCount = meshCount;
    model.materialCount = (int)m3d->nummaterial + 1;
    model.meshes = (Mesh *)RL_CALLOC((size_t)meshCount, sizeof(Mesh));
    model.meshMaterial = (int *)RL_CALLOC((size_t)meshCount, sizeof(int));
    model.materials = (Material *)RL_CALLOC((size_t)model.materialCount, sizeof(Material));

    if ((model.meshes == NULL) || (model.meshMaterial == NULL) || (model.materials == NULL))
    {
        RL_FREE(model.meshes);
        RL_FREE(model.meshMaterial);
        RL_FREE(model.materials);
        m3d_free(m3d);
        UnloadFileData(fileData);
        return (Model){ 0 };
    }

    LoadMaterialsM3D(&model, m3d);
    LoadSkeletonM3D(&model, m3d);

    // Fill the meshes, group by group
    int meshIndex = -1;
    int vertexIndex = 0;
    lastMaterial = M3D_UNDEF - 1;

    for (unsigned int i = 0; i < m3d->numface; i++)
    {
        if (m3d->face[i].materialid != lastMaterial)
        {
            lastMaterial = m3d->face[i].materialid;
            meshIndex++;
            vertexIndex = 0;

            // Count the faces of this material group
            int groupFaces = 0;
            for (unsigned int j = i; (j < m3d->numface) && (m3d->face[j].materialid == lastMaterial); j++) groupFaces++;

            Mesh *mesh = &model.meshes[meshIndex];
            mesh->triangleCount = groupFaces;
            mesh->vertexCount = groupFaces*3;
            mesh->vertices = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
            mesh->texcoords = (float *)RL_CALLOC((size_t)mesh->vertexCount*2, sizeof(float));
            mesh->normals = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
            mesh->boneCount = (int)model.skeleton.boneCount;

            if (hasVertexColors) mesh->colors = (unsigned char *)RL_CALLOC((size_t)mesh->vertexCount*4, sizeof(unsigned char));

            if (hasSkin)
            {
                mesh->boneIndices = (unsigned char *)RL_CALLOC((size_t)mesh->vertexCount*4, sizeof(unsigned char));
                mesh->boneWeights = (float *)RL_CALLOC((size_t)mesh->vertexCount*4, sizeof(float));
                mesh->animVertices = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
                mesh->animNormals = (float *)RL_CALLOC((size_t)mesh->vertexCount*3, sizeof(float));
            }

            model.meshMaterial[meshIndex] = (lastMaterial == M3D_UNDEF)? 0 : ((int)lastMaterial + 1);
        }

        Mesh *mesh = &model.meshes[meshIndex];
        if (mesh->vertices == NULL) continue;

        for (int k = 0; k < 3; k++)
        {
            const M3D_INDEX vid = m3d->face[i].vertex[k];
            const int v = vertexIndex++;

            if (vid >= m3d->numvertex) continue;

            mesh->vertices[v*3] = (float)m3d->vertex[vid].x*m3d->scale;
            mesh->vertices[v*3 + 1] = (float)m3d->vertex[vid].y*m3d->scale;
            mesh->vertices[v*3 + 2] = (float)m3d->vertex[vid].z*m3d->scale;

            if ((mesh->colors != NULL))
            {
                const Color c = ColorFromM3D(m3d->vertex[vid].color);
                mesh->colors[v*4] = c.r;
                mesh->colors[v*4 + 1] = c.g;
                mesh->colors[v*4 + 2] = c.b;
                mesh->colors[v*4 + 3] = c.a;
            }

            const M3D_INDEX tid = m3d->face[i].texcoord[k];
            if ((mesh->texcoords != NULL) && (tid != M3D_UNDEF) && (tid < m3d->numtmap))
            {
                mesh->texcoords[v*2] = (float)m3d->tmap[tid].u;
                mesh->texcoords[v*2 + 1] = 1.0f - (float)m3d->tmap[tid].v;
            }

            const M3D_INDEX nid = m3d->face[i].normal[k];
            if ((mesh->normals != NULL) && (nid != M3D_UNDEF) && (nid < m3d->numvertex))
            {
                mesh->normals[v*3] = (float)m3d->vertex[nid].x;
                mesh->normals[v*3 + 1] = (float)m3d->vertex[nid].y;
                mesh->normals[v*3 + 2] = (float)m3d->vertex[nid].z;
            }

            if (hasSkin && (mesh->boneIndices != NULL) && (mesh->boneWeights != NULL))
            {
                const M3D_INDEX sid = m3d->vertex[vid].skinid;

                if ((sid != M3D_UNDEF) && (sid < m3d->numskin))
                {
                    for (int n = 0; n < 4; n++)
                    {
                        const M3D_INDEX boneId = m3d->skin[sid].boneid[n];
                        if ((boneId == M3D_UNDEF) || (boneId >= m3d->numbone)) break;

                        mesh->boneIndices[v*4 + n] = (unsigned char)boneId;
                        mesh->boneWeights[v*4 + n] = (float)m3d->skin[sid].weight[n];
                    }
                }
                else
                {
                    // Assign the special "no bone" bone with full weight
                    mesh->boneIndices[v*4] = (unsigned char)m3d->numbone;
                    mesh->boneWeights[v*4] = 1.0f;
                }
            }
        }
    }

    // Seed the animated vertex arrays with the bind pose data
    for (int m = 0; m < model.meshCount; m++)
    {
        Mesh *mesh = &model.meshes[m];

        if ((mesh->animVertices != NULL) && (mesh->vertices != NULL))
        {
            memcpy(mesh->animVertices, mesh->vertices, (size_t)mesh->vertexCount*3*sizeof(float));
        }

        if ((mesh->animNormals != NULL) && (mesh->normals != NULL))
        {
            memcpy(mesh->animNormals, mesh->normals, (size_t)mesh->vertexCount*3*sizeof(float));
        }
    }

    m3d_free(m3d);
    UnloadFileData(fileData);

    return model;
}

// Load M3D animation data
static ModelAnimation *LoadModelAnimationsM3D(const char *fileName, int *animCount)
{
    if (animCount != NULL) *animCount = 0;

    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);
    if (fileData == NULL) return NULL;

    m3d_t *m3d = m3d_load(fileData, NULL, NULL, NULL);

    if ((m3d == NULL) || M3D_ERR_ISFATAL(m3d->errcode) || (m3d->numaction == 0) || (m3d->numbone == 0))
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] M3D data does not contain animations", fileName);
        if (m3d != NULL) m3d_free(m3d);
        UnloadFileData(fileData);
        return NULL;
    }

    const unsigned int boneCount = m3d->numbone + 1;

    ModelAnimation *animations = (ModelAnimation *)RL_CALLOC(m3d->numaction, sizeof(ModelAnimation));
    BoneInfo *bones = (BoneInfo *)RL_CALLOC(boneCount, sizeof(BoneInfo));

    if ((animations == NULL) || (bones == NULL))
    {
        RL_FREE(animations);
        RL_FREE(bones);
        m3d_free(m3d);
        UnloadFileData(fileData);
        return NULL;
    }

    for (unsigned int i = 0; i < m3d->numbone; i++)
    {
        bones[i].parent = (m3d->bone[i].parent == M3D_UNDEF)? -1 : (int)m3d->bone[i].parent;
    }
    bones[m3d->numbone].parent = -1;

    for (unsigned int a = 0; a < m3d->numaction; a++)
    {
        ModelAnimation *animation = &animations[a];

        animation->boneCount = boneCount;
        animation->keyframeCount = (int)(m3d->action[a].durationmsec/M3D_ANIMDELAY);
        if (animation->keyframeCount < 1) animation->keyframeCount = 1;

        if (m3d->action[a].name != NULL) strncpy(animation->name, m3d->action[a].name, sizeof(animation->name) - 1);

        animation->keyframePoses = (Transform **)RL_CALLOC((size_t)animation->keyframeCount, sizeof(Transform *));
        if (animation->keyframePoses == NULL)
        {
            animation->keyframeCount = 0;
            continue;
        }

        for (int frame = 0; frame < animation->keyframeCount; frame++)
        {
            animation->keyframePoses[frame] = (Transform *)RL_CALLOC(boneCount, sizeof(Transform));
            if (animation->keyframePoses[frame] == NULL) continue;

            Transform *pose = animation->keyframePoses[frame];

            // The "no bone" entry is always the identity transform
            pose[m3d->numbone].rotation = QuaternionIdentity();
            pose[m3d->numbone].scale = (Vector3){ 1.0f, 1.0f, 1.0f };

            m3db_t *skeleton = m3d_pose(m3d, (M3D_INDEX)a, (uint32_t)(frame*M3D_ANIMDELAY));
            if (skeleton == NULL) continue;

            for (unsigned int i = 0; i < m3d->numbone; i++)
            {
                const m3dv_t *pos = &m3d->vertex[skeleton[i].pos];
                const m3dv_t *ori = &m3d->vertex[skeleton[i].ori];

                pose[i].translation = (Vector3){ (float)pos->x*m3d->scale, (float)pos->y*m3d->scale, (float)pos->z*m3d->scale };
                pose[i].rotation = QuaternionNormalize((Quaternion){ (float)ori->x, (float)ori->y, (float)ori->z, (float)ori->w });
                pose[i].scale = (Vector3){ 1.0f, 1.0f, 1.0f };

                // Child bones are stored in parent bone relative space, convert that into model space
                M3DComposeWithParent(pose, bones, (int)i);
            }

            RL_FREE(skeleton);
        }
    }

    RL_FREE(bones);

    if (animCount != NULL) *animCount = (int)m3d->numaction;

    m3d_free(m3d);
    UnloadFileData(fileData);

    return animations;
}
#else
static Model LoadM3D(const char *fileName) { (void)fileName; return (Model){ 0 }; }
static ModelAnimation *LoadModelAnimationsM3D(const char *fileName, int *animCount) { (void)fileName; if (animCount != NULL) *animCount = 0; return NULL; }
#endif

#if defined(SUPPORT_FILEFORMAT_GLTF)
#define GLTF_ANIMDELAY  17      // Animation frame delay in ms (roughly 60 fps)

// Build a raylib Matrix out of a cgltf column-major 4x4 float array
static Matrix MatrixFromGLTF(const cgltf_float *m)
{
    const Matrix result = {
        m[0], m[4], m[8], m[12],
        m[1], m[5], m[9], m[13],
        m[2], m[6], m[10], m[14],
        m[3], m[7], m[11], m[15]
    };

    return result;
}

// Unpack an accessor into a newly allocated float array
// NOTE: Returns NULL when the accessor is missing or its shape does not match
static float *LoadAccessorFloatsGLTF(const cgltf_accessor *accessor, int components, int count)
{
    if ((accessor == NULL) || (count <= 0)) return NULL;
    if ((int)cgltf_num_components(accessor->type) != components) return NULL;
    if ((int)accessor->count != count) return NULL;

    float *values = (float *)RL_MALLOC((size_t)count*components*sizeof(float));
    if (values == NULL) return NULL;

    if (cgltf_accessor_unpack_floats(accessor, values, (cgltf_size)count*components) == 0)
    {
        RL_FREE(values);
        return NULL;
    }

    return values;
}

// Load a glTF image (external file, embedded buffer view or data URI) as a raylib texture
static Texture2D LoadTextureFromGLTFImage(const cgltf_image *image, const char *basePath)
{
    Texture2D texture = { 0 };

    if (image == NULL) return texture;

    Image rimage = { 0 };

    if (image->uri != NULL)
    {
        if (strncmp(image->uri, "data:", 5) == 0)
        {
            // Data URI: data:<mediatype>;base64,<payload>
            const char *payload = strstr(image->uri, "base64,");
            if (payload == NULL) return texture;
            payload += 7;

            int outputSize = 0;
            unsigned char *data = DecodeDataBase64(payload, &outputSize);
            if (data == NULL) return texture;

            rimage = LoadImageFromMemory(".png", data, outputSize);
            MemFree(data);
        }
        else if ((basePath != NULL) && (basePath[0] != '\0')) rimage = LoadImage(TextFormat("%s/%s", basePath, image->uri));
        else rimage = LoadImage(image->uri);
    }
    else if ((image->buffer_view != NULL) && (image->buffer_view->buffer != NULL) && (image->buffer_view->buffer->data != NULL))
    {
        const unsigned char *src = (const unsigned char *)image->buffer_view->buffer->data + image->buffer_view->offset;
        rimage = LoadImageFromMemory(".png", src, (int)image->buffer_view->size);
    }

    if (rimage.data != NULL)
    {
        texture = LoadTextureFromImage(rimage);
        UnloadImage(rimage);
    }

    return texture;
}

// Fill one raylib material out of a glTF material
static void LoadMaterialGLTF(Material *material, const cgltf_material *src, const char *basePath)
{
    *material = LoadMaterialDefault();
    if (material->maps == NULL) return;

    if (src->has_pbr_metallic_roughness)
    {
        const cgltf_pbr_metallic_roughness *pbr = &src->pbr_metallic_roughness;

        material->maps[MATERIAL_MAP_ALBEDO].color = (Color){
            (unsigned char)(Clamp(pbr->base_color_factor[0], 0.0f, 1.0f)*255.0f),
            (unsigned char)(Clamp(pbr->base_color_factor[1], 0.0f, 1.0f)*255.0f),
            (unsigned char)(Clamp(pbr->base_color_factor[2], 0.0f, 1.0f)*255.0f),
            (unsigned char)(Clamp(pbr->base_color_factor[3], 0.0f, 1.0f)*255.0f) };

        if (pbr->base_color_texture.texture != NULL)
        {
            const Texture2D texture = LoadTextureFromGLTFImage(pbr->base_color_texture.texture->image, basePath);
            if (texture.id > 0) material->maps[MATERIAL_MAP_ALBEDO].texture = texture;
        }

        material->maps[MATERIAL_MAP_METALNESS].value = pbr->metallic_factor;
        material->maps[MATERIAL_MAP_ROUGHNESS].value = pbr->roughness_factor;

        if (pbr->metallic_roughness_texture.texture != NULL)
        {
            const Texture2D texture = LoadTextureFromGLTFImage(pbr->metallic_roughness_texture.texture->image, basePath);
            if (texture.id > 0) material->maps[MATERIAL_MAP_METALNESS].texture = texture;
        }
    }

    if (src->normal_texture.texture != NULL)
    {
        const Texture2D texture = LoadTextureFromGLTFImage(src->normal_texture.texture->image, basePath);
        if (texture.id > 0) material->maps[MATERIAL_MAP_NORMAL].texture = texture;
    }

    if (src->occlusion_texture.texture != NULL)
    {
        const Texture2D texture = LoadTextureFromGLTFImage(src->occlusion_texture.texture->image, basePath);
        if (texture.id > 0) material->maps[MATERIAL_MAP_OCCLUSION].texture = texture;
    }

    if (src->emissive_texture.texture != NULL)
    {
        const Texture2D texture = LoadTextureFromGLTFImage(src->emissive_texture.texture->image, basePath);
        if (texture.id > 0) material->maps[MATERIAL_MAP_EMISSION].texture = texture;
    }

    material->maps[MATERIAL_MAP_EMISSION].color = (Color){
        (unsigned char)(Clamp(src->emissive_factor[0], 0.0f, 1.0f)*255.0f),
        (unsigned char)(Clamp(src->emissive_factor[1], 0.0f, 1.0f)*255.0f),
        (unsigned char)(Clamp(src->emissive_factor[2], 0.0f, 1.0f)*255.0f), 255 };
}

// Find the index of a node inside a skin joints array, -1 when not found
static int FindJointIndexGLTF(const cgltf_skin *skin, const cgltf_node *node)
{
    if ((skin == NULL) || (node == NULL)) return -1;

    for (cgltf_size i = 0; i < skin->joints_count; i++)
    {
        if (skin->joints[i] == node) return (int)i;
    }

    return -1;
}

// Load the vertex attributes of one glTF primitive into a raylib mesh
static void LoadPrimitiveGLTF(Mesh *mesh, const cgltf_primitive *primitive, const cgltf_skin *skin)
{
    // Vertex count comes from the POSITION attribute, which is mandatory
    for (cgltf_size a = 0; a < primitive->attributes_count; a++)
    {
        if (primitive->attributes[a].type != cgltf_attribute_type_position) continue;
        mesh->vertexCount = (int)primitive->attributes[a].data->count;
        break;
    }

    if (mesh->vertexCount <= 0) return;

    const int vertexCount = mesh->vertexCount;

    for (cgltf_size a = 0; a < primitive->attributes_count; a++)
    {
        const cgltf_attribute *attribute = &primitive->attributes[a];
        const cgltf_accessor *accessor = attribute->data;

        switch (attribute->type)
        {
            case cgltf_attribute_type_position:
            {
                mesh->vertices = LoadAccessorFloatsGLTF(accessor, 3, vertexCount);
            } break;
            case cgltf_attribute_type_normal:
            {
                mesh->normals = LoadAccessorFloatsGLTF(accessor, 3, vertexCount);
            } break;
            case cgltf_attribute_type_tangent:
            {
                mesh->tangents = LoadAccessorFloatsGLTF(accessor, 4, vertexCount);
            } break;
            case cgltf_attribute_type_texcoord:
            {
                float *uv = LoadAccessorFloatsGLTF(accessor, 2, vertexCount);
                if (uv == NULL) break;

                if ((attribute->index == 0) && (mesh->texcoords == NULL)) mesh->texcoords = uv;
                else if ((attribute->index == 1) && (mesh->texcoords2 == NULL)) mesh->texcoords2 = uv;
                else RL_FREE(uv);
            } break;
            case cgltf_attribute_type_color:
            {
                if (mesh->colors != NULL) break;

                const int components = (int)cgltf_num_components(accessor->type);
                if ((components != 3) && (components != 4)) break;

                float *values = LoadAccessorFloatsGLTF(accessor, components, vertexCount);
                if (values == NULL) break;

                mesh->colors = (unsigned char *)RL_MALLOC((size_t)vertexCount*4*sizeof(unsigned char));

                if (mesh->colors != NULL)
                {
                    for (int v = 0; v < vertexCount; v++)
                    {
                        mesh->colors[v*4] = (unsigned char)(Clamp(values[v*components], 0.0f, 1.0f)*255.0f);
                        mesh->colors[v*4 + 1] = (unsigned char)(Clamp(values[v*components + 1], 0.0f, 1.0f)*255.0f);
                        mesh->colors[v*4 + 2] = (unsigned char)(Clamp(values[v*components + 2], 0.0f, 1.0f)*255.0f);
                        mesh->colors[v*4 + 3] = (components == 4)? (unsigned char)(Clamp(values[v*components + 3], 0.0f, 1.0f)*255.0f) : 255;
                    }
                }

                RL_FREE(values);
            } break;
            case cgltf_attribute_type_joints:
            {
                if ((skin == NULL) || (attribute->index != 0) || (mesh->boneIndices != NULL)) break;
                if (cgltf_num_components(accessor->type) != 4) break;

                mesh->boneIndices = (unsigned char *)RL_CALLOC((size_t)vertexCount*4, sizeof(unsigned char));
                if (mesh->boneIndices == NULL) break;

                for (int v = 0; v < vertexCount; v++)
                {
                    cgltf_uint joints[4] = { 0 };

                    if (cgltf_accessor_read_uint(accessor, (cgltf_size)v, joints, 4) == 0) continue;

                    for (int k = 0; k < 4; k++)
                    {
                        mesh->boneIndices[v*4 + k] = (joints[k] < 255)? (unsigned char)joints[k] : 0;
                    }
                }
            } break;
            case cgltf_attribute_type_weights:
            {
                if ((skin == NULL) || (attribute->index != 0) || (mesh->boneWeights != NULL)) break;
                mesh->boneWeights = LoadAccessorFloatsGLTF(accessor, 4, vertexCount);
            } break;
            default: break;
        }
    }

    // Indices
    if (primitive->indices != NULL)
    {
        mesh->triangleCount = (int)primitive->indices->count/3;
        mesh->indices = (unsigned short *)RL_MALLOC((size_t)mesh->triangleCount*3*sizeof(unsigned short));

        if (mesh->indices != NULL)
        {
            for (int i = 0; i < mesh->triangleCount*3; i++)
            {
                mesh->indices[i] = (unsigned short)cgltf_accessor_read_index(primitive->indices, (cgltf_size)i);
            }
        }
    }
    else mesh->triangleCount = vertexCount/3;

    // Runtime skinning buffers
    if ((mesh->boneIndices != NULL) && (mesh->boneWeights != NULL))
    {
        mesh->boneCount = (skin != NULL)? (int)skin->joints_count : 0;

        if (mesh->vertices != NULL)
        {
            mesh->animVertices = (float *)RL_MALLOC((size_t)vertexCount*3*sizeof(float));
            if (mesh->animVertices != NULL) memcpy(mesh->animVertices, mesh->vertices, (size_t)vertexCount*3*sizeof(float));
        }

        if (mesh->normals != NULL)
        {
            mesh->animNormals = (float *)RL_MALLOC((size_t)vertexCount*3*sizeof(float));
            if (mesh->animNormals != NULL) memcpy(mesh->animNormals, mesh->normals, (size_t)vertexCount*3*sizeof(float));
        }
    }
}

// Apply a node world transform to the vertex positions and normals of a mesh
static void TransformMeshGLTF(Mesh *mesh, Matrix transform)
{
    if (mesh->vertices == NULL) return;

    Matrix rotation = transform;
    rotation.m12 = 0.0f;
    rotation.m13 = 0.0f;
    rotation.m14 = 0.0f;

    for (int v = 0; v < mesh->vertexCount; v++)
    {
        const Vector3 position = Vector3Transform((Vector3){ mesh->vertices[v*3], mesh->vertices[v*3 + 1], mesh->vertices[v*3 + 2] }, transform);
        mesh->vertices[v*3] = position.x;
        mesh->vertices[v*3 + 1] = position.y;
        mesh->vertices[v*3 + 2] = position.z;

        if (mesh->normals != NULL)
        {
            const Vector3 normal = Vector3Normalize(Vector3Transform((Vector3){ mesh->normals[v*3], mesh->normals[v*3 + 1], mesh->normals[v*3 + 2] }, rotation));
            mesh->normals[v*3] = normal.x;
            mesh->normals[v*3 + 1] = normal.y;
            mesh->normals[v*3 + 2] = normal.z;
        }
    }

    if (mesh->animVertices != NULL) memcpy(mesh->animVertices, mesh->vertices, (size_t)mesh->vertexCount*3*sizeof(float));
    if ((mesh->animNormals != NULL) && (mesh->normals != NULL)) memcpy(mesh->animNormals, mesh->normals, (size_t)mesh->vertexCount*3*sizeof(float));
}

// Load the skeleton described by the first glTF skin
static void LoadSkeletonGLTF(Model *model, const cgltf_skin *skin)
{
    if ((skin == NULL) || (skin->joints_count == 0)) return;

    const unsigned int boneCount = (unsigned int)skin->joints_count;

    model->skeleton.bones = (BoneInfo *)RL_CALLOC(boneCount, sizeof(BoneInfo));
    model->skeleton.bindPose = (Transform *)RL_CALLOC(boneCount, sizeof(Transform));

    if ((model->skeleton.bones == NULL) || (model->skeleton.bindPose == NULL))
    {
        RL_FREE(model->skeleton.bones);
        RL_FREE(model->skeleton.bindPose);
        model->skeleton = (ModelSkeleton){ 0 };
        return;
    }

    model->skeleton.boneCount = boneCount;

    for (unsigned int i = 0; i < boneCount; i++)
    {
        const cgltf_node *node = skin->joints[i];

        if ((node != NULL) && (node->name != NULL)) strncpy(model->skeleton.bones[i].name, node->name, sizeof(model->skeleton.bones[i].name) - 1);
        model->skeleton.bones[i].parent = (node != NULL)? FindJointIndexGLTF(skin, node->parent) : -1;

        Transform bind = { { 0.0f, 0.0f, 0.0f }, QuaternionIdentity(), { 1.0f, 1.0f, 1.0f } };

        if (node != NULL)
        {
            if (node->has_translation) bind.translation = (Vector3){ node->translation[0], node->translation[1], node->translation[2] };
            if (node->has_rotation) bind.rotation = QuaternionNormalize((Quaternion){ node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3] });
            if (node->has_scale) bind.scale = (Vector3){ node->scale[0], node->scale[1], node->scale[2] };
        }

        model->skeleton.bindPose[i] = bind;
    }

    // Node transforms are parent relative, turn them into model space
    BuildPoseFromParentJoints(model->skeleton.bones, boneCount, model->skeleton.bindPose);
}

// Load glTF/GLB mesh data
static Model LoadGLTF(const char *fileName)
{
    Model model = { 0 };
    model.transform = MatrixIdentity();

    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);
    if (fileData == NULL) return model;

    cgltf_options options = { 0 };
    cgltf_data *data = NULL;

    if (cgltf_parse(&options, fileData, (cgltf_size)dataSize, &data) != cgltf_result_success)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] Failed to load glTF data", fileName);
        UnloadFileData(fileData);
        return model;
    }

    if (cgltf_load_buffers(&options, data, fileName) != cgltf_result_success)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] Failed to load glTF buffers", fileName);
        cgltf_free(data);
        UnloadFileData(fileData);
        return model;
    }

    TRACELOG(LOG_INFO, "MODEL: [%s] glTF data loaded successfully: %i meshes/%i materials",
             fileName, (int)data->meshes_count, (int)data->materials_count);

    // Count the triangle primitives, one raylib mesh per primitive
    int meshCount = 0;
    for (cgltf_size m = 0; m < data->meshes_count; m++)
    {
        for (cgltf_size p = 0; p < data->meshes[m].primitives_count; p++)
        {
            if (data->meshes[m].primitives[p].type == cgltf_primitive_type_triangles) meshCount++;
        }
    }

    if (meshCount == 0)
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] glTF data contains no triangle primitives", fileName);
        cgltf_free(data);
        UnloadFileData(fileData);
        return model;
    }

    // Material slot 0 is the default material (raylib convention), glTF at 1..N
    model.meshCount = meshCount;
    model.materialCount = (int)data->materials_count + 1;
    model.meshes = (Mesh *)RL_CALLOC((size_t)meshCount, sizeof(Mesh));
    model.meshMaterial = (int *)RL_CALLOC((size_t)meshCount, sizeof(int));
    model.materials = (Material *)RL_CALLOC((size_t)model.materialCount, sizeof(Material));

    int *meshBaseByGltfMesh = (int *)RL_CALLOC((size_t)((data->meshes_count > 0)? data->meshes_count : 1), sizeof(int));

    if ((model.meshes == NULL) || (model.meshMaterial == NULL) || (model.materials == NULL) || (meshBaseByGltfMesh == NULL))
    {
        RL_FREE(model.meshes);
        RL_FREE(model.meshMaterial);
        RL_FREE(model.materials);
        RL_FREE(meshBaseByGltfMesh);
        cgltf_free(data);
        UnloadFileData(fileData);
        return (Model){ 0 };
    }

    const char *basePath = GetDirectoryPath(fileName);

    // raylib convention: materials[0] is the default material, glTF materials
    // start at index 1 (upstream examples rely on this, e.g. models_bone_socket
    // draws with materials[1])
    model.materials[0] = LoadMaterialDefault();
    for (cgltf_size i = 0; i < data->materials_count; i++) LoadMaterialGLTF(&model.materials[i + 1], &data->materials[i], basePath);

    const cgltf_skin *skin = (data->skins_count > 0)? &data->skins[0] : NULL;

    // Load the primitives
    int meshIndex = 0;
    for (cgltf_size m = 0; m < data->meshes_count; m++)
    {
        meshBaseByGltfMesh[m] = meshIndex;

        for (cgltf_size p = 0; p < data->meshes[m].primitives_count; p++)
        {
            const cgltf_primitive *primitive = &data->meshes[m].primitives[p];
            if (primitive->type != cgltf_primitive_type_triangles) continue;

            LoadPrimitiveGLTF(&model.meshes[meshIndex], primitive, skin);

            // +1: glTF material i lives at materials[i + 1], materials[0] is the default
            const int materialIndex = (primitive->material != NULL)?
                ((int)cgltf_material_index(data, primitive->material) + 1) : 0;
            model.meshMaterial[meshIndex] = ((materialIndex >= 1) && (materialIndex < model.materialCount))? materialIndex : 0;

            meshIndex++;
        }
    }

    // Apply the node world transforms to the static (non skinned) meshes
    for (cgltf_size n = 0; n < data->nodes_count; n++)
    {
        const cgltf_node *node = &data->nodes[n];
        if (node->mesh == NULL) continue;
        if (node->skin != NULL) continue;    // Skinned meshes are posed by the skeleton

        const cgltf_size gltfMeshIndex = cgltf_mesh_index(data, node->mesh);
        if (gltfMeshIndex >= data->meshes_count) continue;

        cgltf_float worldTransform[16] = { 0 };
        cgltf_node_transform_world(node, worldTransform);
        const Matrix transform = MatrixFromGLTF(worldTransform);

        int primitiveIndex = meshBaseByGltfMesh[gltfMeshIndex];

        for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
        {
            if (node->mesh->primitives[p].type != cgltf_primitive_type_triangles) continue;
            if (primitiveIndex >= model.meshCount) break;

            TransformMeshGLTF(&model.meshes[primitiveIndex], transform);
            primitiveIndex++;
        }
    }

    LoadSkeletonGLTF(&model, skin);

    RL_FREE(meshBaseByGltfMesh);

    cgltf_free(data);
    UnloadFileData(fileData);

    return model;
}

// Sample an animation channel at the given time
// NOTE: out must hold 'components' floats; rotations (4 components) are slerped
static bool SampleChannelGLTF(const cgltf_animation_sampler *sampler, float time, float *out, int components, bool isRotation)
{
    if ((sampler == NULL) || (sampler->input == NULL) || (sampler->output == NULL)) return false;

    const cgltf_size keyCount = sampler->input->count;
    if (keyCount == 0) return false;

    const bool cubic = (sampler->interpolation == cgltf_interpolation_type_cubic_spline);
    const cgltf_size stride = cubic? 3 : 1;
    const cgltf_size valueOffset = cubic? 1 : 0;

    // Locate the keyframe interval containing 'time'
    cgltf_size key = 0;
    float t0 = 0.0f;
    float t1 = 0.0f;

    if (!cgltf_accessor_read_float(sampler->input, 0, &t0, 1)) return false;

    if (time <= t0) return (cgltf_accessor_read_float(sampler->output, valueOffset, out, (cgltf_size)components) != 0);

    if (!cgltf_accessor_read_float(sampler->input, keyCount - 1, &t1, 1)) return false;

    if ((time >= t1) || (keyCount == 1))
    {
        return (cgltf_accessor_read_float(sampler->output, (keyCount - 1)*stride + valueOffset, out, (cgltf_size)components) != 0);
    }

    for (cgltf_size i = 0; i < keyCount - 1; i++)
    {
        float a = 0.0f;
        float b = 0.0f;
        if (!cgltf_accessor_read_float(sampler->input, i, &a, 1)) return false;
        if (!cgltf_accessor_read_float(sampler->input, i + 1, &b, 1)) return false;

        if ((time >= a) && (time <= b)) { key = i; t0 = a; t1 = b; break; }
    }

    float v0[4] = { 0 };
    float v1[4] = { 0 };

    if (!cgltf_accessor_read_float(sampler->output, key*stride + valueOffset, v0, (cgltf_size)components)) return false;
    if (!cgltf_accessor_read_float(sampler->output, (key + 1)*stride + valueOffset, v1, (cgltf_size)components)) return false;

    if (sampler->interpolation == cgltf_interpolation_type_step)
    {
        memcpy(out, v0, (size_t)components*sizeof(float));
        return true;
    }

    // NOTE: Cubic spline tangents are not evaluated, values are interpolated linearly
    const float span = t1 - t0;
    const float t = (span > 0.0f)? (time - t0)/span : 0.0f;

    if (isRotation && (components == 4))
    {
        const Quaternion q = QuaternionSlerp((Quaternion){ v0[0], v0[1], v0[2], v0[3] }, (Quaternion){ v1[0], v1[1], v1[2], v1[3] }, t);
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    }
    else
    {
        for (int i = 0; i < components; i++) out[i] = v0[i] + (v1[i] - v0[i])*t;
    }

    return true;
}

// Load glTF/GLB animation data
static ModelAnimation *LoadModelAnimationsGLTF(const char *fileName, int *animCount)
{
    if (animCount != NULL) *animCount = 0;

    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);
    if (fileData == NULL) return NULL;

    cgltf_options options = { 0 };
    cgltf_data *data = NULL;

    if (cgltf_parse(&options, fileData, (cgltf_size)dataSize, &data) != cgltf_result_success)
    {
        UnloadFileData(fileData);
        return NULL;
    }

    if ((cgltf_load_buffers(&options, data, fileName) != cgltf_result_success) ||
        (data->animations_count == 0) || (data->skins_count == 0))
    {
        TRACELOG(LOG_WARNING, "MODEL: [%s] glTF data does not contain animations", fileName);
        cgltf_free(data);
        UnloadFileData(fileData);
        return NULL;
    }

    const cgltf_skin *skin = &data->skins[0];
    const unsigned int boneCount = (unsigned int)skin->joints_count;

    ModelAnimation *animations = (ModelAnimation *)RL_CALLOC(data->animations_count, sizeof(ModelAnimation));
    BoneInfo *bones = (BoneInfo *)RL_CALLOC(boneCount, sizeof(BoneInfo));

    if ((animations == NULL) || (bones == NULL) || (boneCount == 0))
    {
        RL_FREE(animations);
        RL_FREE(bones);
        cgltf_free(data);
        UnloadFileData(fileData);
        return NULL;
    }

    for (unsigned int i = 0; i < boneCount; i++) bones[i].parent = FindJointIndexGLTF(skin, skin->joints[i]->parent);

    for (cgltf_size a = 0; a < data->animations_count; a++)
    {
        const cgltf_animation *anim = &data->animations[a];
        ModelAnimation *animation = &animations[a];

        if (anim->name != NULL) strncpy(animation->name, anim->name, sizeof(animation->name) - 1);

        // Animation duration is the largest keyframe timestamp across all its channels
        float duration = 0.0f;
        for (cgltf_size c = 0; c < anim->channels_count; c++)
        {
            const cgltf_animation_sampler *sampler = anim->channels[c].sampler;
            if ((sampler == NULL) || (sampler->input == NULL) || (sampler->input->count == 0)) continue;

            float last = 0.0f;
            if (cgltf_accessor_read_float(sampler->input, sampler->input->count - 1, &last, 1) && (last > duration)) duration = last;
        }

        animation->boneCount = boneCount;
        animation->keyframeCount = (int)(duration*1000.0f/(float)GLTF_ANIMDELAY) + 1;
        animation->keyframePoses = (Transform **)RL_CALLOC((size_t)animation->keyframeCount, sizeof(Transform *));

        if (animation->keyframePoses == NULL)
        {
            animation->keyframeCount = 0;
            continue;
        }

        for (int frame = 0; frame < animation->keyframeCount; frame++)
        {
            Transform *pose = (Transform *)RL_CALLOC(boneCount, sizeof(Transform));
            animation->keyframePoses[frame] = pose;
            if (pose == NULL) continue;

            const float time = (float)frame*(float)GLTF_ANIMDELAY/1000.0f;

            // Start from the joints rest (parent relative) transforms
            for (unsigned int i = 0; i < boneCount; i++)
            {
                const cgltf_node *node = skin->joints[i];

                pose[i].translation = node->has_translation? (Vector3){ node->translation[0], node->translation[1], node->translation[2] } : (Vector3){ 0.0f, 0.0f, 0.0f };
                pose[i].rotation = node->has_rotation? (Quaternion){ node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3] } : QuaternionIdentity();
                pose[i].scale = node->has_scale? (Vector3){ node->scale[0], node->scale[1], node->scale[2] } : (Vector3){ 1.0f, 1.0f, 1.0f };
            }

            // Override with the animated channels
            for (cgltf_size c = 0; c < anim->channels_count; c++)
            {
                const cgltf_animation_channel *channel = &anim->channels[c];

                const int boneId = FindJointIndexGLTF(skin, channel->target_node);
                if (boneId < 0) continue;

                float values[4] = { 0 };

                switch (channel->target_path)
                {
                    case cgltf_animation_path_type_translation:
                    {
                        if (SampleChannelGLTF(channel->sampler, time, values, 3, false))
                        {
                            pose[boneId].translation = (Vector3){ values[0], values[1], values[2] };
                        }
                    } break;
                    case cgltf_animation_path_type_rotation:
                    {
                        if (SampleChannelGLTF(channel->sampler, time, values, 4, true))
                        {
                            pose[boneId].rotation = (Quaternion){ values[0], values[1], values[2], values[3] };
                        }
                    } break;
                    case cgltf_animation_path_type_scale:
                    {
                        if (SampleChannelGLTF(channel->sampler, time, values, 3, false))
                        {
                            pose[boneId].scale = (Vector3){ values[0], values[1], values[2] };
                        }
                    } break;
                    default: break;
                }
            }

            for (unsigned int i = 0; i < boneCount; i++) pose[i].rotation = QuaternionNormalize(pose[i].rotation);

            // Node transforms are parent relative, turn them into model space
            BuildPoseFromParentJoints(bones, boneCount, pose);
        }
    }

    RL_FREE(bones);

    if (animCount != NULL) *animCount = (int)data->animations_count;

    cgltf_free(data);
    UnloadFileData(fileData);

    return animations;
}
#else
static Model LoadGLTF(const char *fileName) { (void)fileName; return (Model){ 0 }; }
static ModelAnimation *LoadModelAnimationsGLTF(const char *fileName, int *animCount) { (void)fileName; if (animCount != NULL) *animCount = 0; return NULL; }
#endif
