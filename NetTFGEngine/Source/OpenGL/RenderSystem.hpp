#ifndef RENDER_SYSTEM_HPP
#define RENDER_SYSTEM_HPP

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "Utils/Debug/Debug.hpp"
#include "Mesh.hpp"
#include "Material.hpp"
#include "OpenGLIncludes.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <iostream>

// -------------------------------------------------------
//  GPU-side light layout — must match light_cull.comp
//  and the SSBO declarations in every .frag shader
// -------------------------------------------------------
struct GPUPointLight {
    glm::vec4 posRadius;      // xyz = world pos,  w = radius
    glm::vec4 colorIntensity; // rgb = color,       a = intensity
};

struct GPUTileData {
    uint32_t offset;
    uint32_t count;
};

// Reemplaza GPUShadowData anterior
struct GPUShadowData {
    glm::mat4 lightSpaceMatrices[6]; // una por cara del cubo
    int       lightIndex;
    float     farPlane;
    int       pad[2];
};



// -------------------------------------------------------
//  Forward+ RenderSystem
//
//  Per-frame pipeline:
//    1. UploadLights   — stream PointLightComponent data → SSBO
//    2. DepthPrePass   — depth-only draw to offscreen FBO
//    3. LightCullPass  — compute shader tiles the screen,
//                        culls lights per tile
//    4. ShadingPass    — normal mesh draw; every frag shader
//                        reads the tile list from SSBOs
// -------------------------------------------------------
class RenderSystem : public ISystem {
public:

    // ---------------------------------------------------
    //  Call once after the OpenGL context is ready.
    //  screenW / screenH must match your actual viewport.
    // ---------------------------------------------------
    void Init(int screenW, int screenH)
    {
        m_screenW = screenW;
        m_screenH = screenH;
        m_tilesX = (screenW + TILE_SIZE - 1) / TILE_SIZE;
        m_tilesY = (screenH + TILE_SIZE - 1) / TILE_SIZE;

        InitDepthFBO();
        InitSSBOs();
        InitShadowCubeArray();
        CompileDepthShader();
        CompileLightCullShader();
        CompileShadowShader();
    }

    // ---------------------------------------------------
    //  Call when the window is resized.
    // ---------------------------------------------------
    void Resize(int screenW, int screenH)
    {
        m_screenW = screenW;
        m_screenH = screenH;
        m_tilesX = (screenW + TILE_SIZE - 1) / TILE_SIZE;
        m_tilesY = (screenH + TILE_SIZE - 1) / TILE_SIZE;

        // --- Depth FBO (screen-size dependent) ---
        glDeleteFramebuffers(1, &m_depthFBO); m_depthFBO = 0;
        glDeleteTextures(1, &m_depthTex);     m_depthTex = 0;
        InitDepthFBO();

        // --- Only the tile-dependent SSBOs need rebuilding ---
        // m_lightSSBO is MAX_LIGHTS fixed size — do NOT touch it
        int totalTiles = m_tilesX * m_tilesY;

        glDeleteBuffers(1, &m_lightIndexSSBO); m_lightIndexSSBO = 0;
        glGenBuffers(1, &m_lightIndexSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightIndexSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(uint32_t) * totalTiles * MAX_LIGHTS_TILE,
            nullptr, GL_DYNAMIC_DRAW);

        glDeleteBuffers(1, &m_tileGridSSBO); m_tileGridSSBO = 0;
        glGenBuffers(1, &m_tileGridSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_tileGridSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(GPUTileData) * totalTiles,
            nullptr, GL_DYNAMIC_DRAW);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override
    {
        if (m_depthFBO == 0) {
            Debug::Error("RenderSystem") << "Call Init() before first Update()\n";
            return;
        }

        // --------------- acquire camera ---------------
        Camera* activeCamera = nullptr;
        Transform* cameraTransform = nullptr;

        entityManager.acquireMutex();

        auto cameraQuery = entityManager.CreateQuery<Camera, Transform>();
        for (auto [entity, camera, transform] : cameraQuery) {
            activeCamera = camera;
            cameraTransform = transform;
            break;
        }

        if (!activeCamera || !cameraTransform) {
            Debug::Warning("RenderSystem") << "No camera found\n";
            entityManager.releaseMutex();
            return;
        }

        glm::mat4 view = activeCamera->getViewMatrix();
        glm::mat4 projection = activeCamera->getProjectionMatrix();
        glm::vec3 cameraPos = cameraTransform->getPosition();

        auto meshQuery = entityManager.CreateQuery<MeshComponent, Transform>();

        // ---- Pass 1: upload lights to SSBO ----
        UploadLights(entityManager);

        ShadowPass(entityManager, meshQuery);

        // ---- Pass 2: depth pre-pass ----
        DepthPrePass(meshQuery, view, projection);

        // ---- Pass 3: light culling compute ----
        LightCullPass(view, projection);

        // ---- Pass 4: full shading ----
        ShadingPass(meshQuery, view, projection, cameraPos);

        entityManager.releaseMutex();
    }

    ~RenderSystem()
    {
        glDeleteFramebuffers(1, &m_depthFBO);
        glDeleteTextures(1, &m_depthTex);
        glDeleteBuffers(1, &m_lightSSBO);
        glDeleteBuffers(1, &m_lightIndexSSBO);
        glDeleteBuffers(1, &m_tileGridSSBO);
        glDeleteProgram(m_depthShader);
        glDeleteProgram(m_lightCullShader);

        // AÑADIR:
        glDeleteFramebuffers(1, &m_shadowFBO);
        glDeleteTextures(1, &m_shadowCubeArray);
        glDeleteBuffers(1, &m_shadowDataSSBO);
        glDeleteProgram(m_shadowShader);
    }

private:
    // =====================================================
    //  Constants
    // =====================================================
    static constexpr int     TILE_SIZE = 16;
    static constexpr int     MAX_LIGHTS = 1024;
    static constexpr int     MAX_LIGHTS_TILE = 256; // per tile
    static constexpr int MAX_SHADOW_LIGHTS = 16; // cubemaps son costosos

    // =====================================================
    //  GPU resource handles
    // =====================================================
    GLuint m_depthFBO = 0;
    GLuint m_depthTex = 0;
    GLuint m_lightSSBO = 0; // binding 0 — PointLight array
    GLuint m_lightIndexSSBO = 0; // binding 1 — flat uint index list
    GLuint m_tileGridSSBO = 0; // binding 2 — (offset, count) per tile
    GLuint m_depthShader = 0; // simple MVP-only program
    GLuint m_lightCullShader = 0; // compute program

    int m_screenW = 0, m_screenH = 0;
    int m_tilesX = 0, m_tilesY = 0;
    int m_lightCount = 0;

    GLuint m_shadowCubeArray = 0;  // GL_TEXTURE_CUBE_MAP_ARRAY
    GLuint m_shadowFBO = 0;
    GLuint m_shadowShader = 0;  // vert + geom + frag
    GLuint m_shadowDataSSBO = 0;
    int m_shadowRes = 512;
    int    m_shadowCount = 0;

    // =====================================================
    //  Initialisation helpers
    // =====================================================
    void InitDepthFBO()
    {
        glGenTextures(1, &m_depthTex);
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
            m_screenW, m_screenH, 0,
            GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenFramebuffers(1, &m_depthFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_TEXTURE_2D, m_depthTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "Depth FBO incomplete\n";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void InitSSBOs()
    {
        int totalTiles = m_tilesX * m_tilesY;

        // Binding 0 — light data (fixed size, updated every frame)
        glGenBuffers(1, &m_lightSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(GPUPointLight) * MAX_LIGHTS,
            nullptr, GL_DYNAMIC_DRAW);

        // Binding 1 — flat list of light indices visible per tile
        glGenBuffers(1, &m_lightIndexSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightIndexSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(uint32_t) * totalTiles * MAX_LIGHTS_TILE,
            nullptr, GL_DYNAMIC_DRAW);

        // Binding 2 — per-tile (offset, count) into the index list
        glGenBuffers(1, &m_tileGridSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_tileGridSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(GPUTileData) * totalTiles,
            nullptr, GL_DYNAMIC_DRAW);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void InitShadowCubeArray()
    {
        static constexpr int SHADOW_RES = 2048;
        m_shadowRes = SHADOW_RES;

        // GL_TEXTURE_CUBE_MAP_ARRAY: profundidad = MAX_SHADOW_LIGHTS * 6
        glGenTextures(1, &m_shadowCubeArray);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);
        glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_DEPTH_COMPONENT32F,
            SHADOW_RES, SHADOW_RES,
            MAX_SHADOW_LIGHTS * 6,  // cada cubemap ocupa 6 capas
            0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        // Un solo FBO — cambiaremos la capa adjunta en cada draw call
        glGenFramebuffers(1, &m_shadowFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
        // Adjuntamos toda la textura; el geometry shader selecciona la capa
        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadowCubeArray, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // SSBO para las matrices y metadatos de sombra
        glGenBuffers(1, &m_shadowDataSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowDataSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            sizeof(GPUShadowData) * MAX_SHADOW_LIGHTS,
            nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // =====================================================
    //  Shader compilation
    // =====================================================
    static GLuint CompileStage(GLenum type, const char* src)
    {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, 512, nullptr, log);
            Debug::Error("RenderSystem::Shader") << log << "\n";
        }
        return s;
    }

    void CompileShadowShader()
    {
        const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec3 aPos;
        uniform mat4 uModel;
        void main() {
            gl_Position = uModel * vec4(aPos, 1.0);
        }
    )GLSL";

        // El geometry shader emite 6 copias del triángulo, una por cara
        const char* geom = R"GLSL(
        #version 430 core
        layout(triangles) in;
        layout(triangle_strip, max_vertices = 18) out;

        uniform mat4 uLightSpaceMatrices[6];
        uniform int  uCubeArrayLayer; // índice base: lightIdx * 6

        out vec4 gFragPos;

        void main() {
            for (int face = 0; face < 6; face++) {
                gl_Layer = uCubeArrayLayer + face; // selecciona capa del array
                for (int v = 0; v < 3; v++) {
                    gFragPos = gl_in[v].gl_Position;
                    gl_Position = uLightSpaceMatrices[face] * gFragPos;
                    EmitVertex();
                }
                EndPrimitive();
            }
        }
    )GLSL";

        const char* frag = R"GLSL(
        #version 430 core
        in  vec4  gFragPos;
        uniform vec3  uLightPos;
        uniform float uFarPlane;

        void main() {
            // Almacenar distancia lineal normalizada en [0, 1]
            float dist = length(gFragPos.xyz - uLightPos) / uFarPlane;
            gl_FragDepth = dist;
        }
    )GLSL";

        m_shadowShader = LinkProgram({
            CompileStage(GL_VERTEX_SHADER,   vert),
            CompileStage(GL_GEOMETRY_SHADER, geom),
            CompileStage(GL_FRAGMENT_SHADER, frag)
            });
    }

    static GLuint LinkProgram(std::initializer_list<GLuint> stages)
    {
        GLuint prog = glCreateProgram();
        for (GLuint s : stages) glAttachShader(prog, s);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(prog, 512, nullptr, log);
            Debug::Error("RenderSystem::Program") << log << "\n";
        }
        for (GLuint s : stages) { glDetachShader(prog, s); glDeleteShader(s); }
        return prog;
    }

    void CompileDepthShader()
    {
        const char* vert = R"GLSL(
            #version 430 core
            layout(location = 0) in vec3 aPos;
            uniform mat4 uMVP;
            void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
        )GLSL";

        const char* frag = R"GLSL(
            #version 430 core
            void main() {}
        )GLSL";

        m_depthShader = LinkProgram({
            CompileStage(GL_VERTEX_SHADER,   vert),
            CompileStage(GL_FRAGMENT_SHADER, frag)
            });
    }

    void CompileLightCullShader()
    {
        // One workgroup = one tile (16×16 threads)
        const char* comp = R"GLSL(
            #version 430 core
            layout(local_size_x = 16, local_size_y = 16) in;

            struct PointLight { vec4 posRadius; vec4 colorIntensity; };
            struct TileData   { uint offset; uint count; };

            layout(std430, binding = 0) readonly  buffer LightBuf  { PointLight lights[]; };
            layout(std430, binding = 1) writeonly buffer LightIdx   { uint lightIndices[]; };
            layout(std430, binding = 2)           buffer TileGrid   { TileData grid[]; };

            uniform sampler2D uDepthMap;
            uniform mat4      uProjection;
            uniform mat4      uView;
            uniform int       uLightCount;
            uniform ivec2     uScreenSize;

            shared uint s_minDepthInt;
            shared uint s_maxDepthInt;
            shared uint s_tileLightCount;
            shared uint s_tileLightIndices[256];

            // Reconstruct a view-space frustum plane from two NDC edge points
            vec4 CreatePlane(vec3 p0, vec3 p1, vec3 p2) {
                vec3 n = normalize(cross(p1 - p0, p2 - p0));
                return vec4(n, -dot(n, p0));
            }

            void main() {
                ivec2 tileID    = ivec2(gl_WorkGroupID.xy);
                ivec2 tileCount = ivec2(gl_NumWorkGroups.xy);
                int   tileIndex = tileID.y * tileCount.x + tileID.x;
                ivec2 pixel     = ivec2(gl_GlobalInvocationID.xy);

                // ---- init shared memory ----
                if (gl_LocalInvocationIndex == 0u) {
                    s_minDepthInt    = 0xFFFFFFFFu;
                    s_maxDepthInt    = 0u;
                    s_tileLightCount = 0u;
                }
                barrier();

                // ---- gather min/max depth for this tile ----
                vec2 uv = (vec2(pixel) + 0.5) / vec2(uScreenSize);
                if (pixel.x < uScreenSize.x && pixel.y < uScreenSize.y) {
                    float depth = texture(uDepthMap, uv).r;
                    atomicMin(s_minDepthInt, floatBitsToUint(depth));
                    atomicMax(s_maxDepthInt, floatBitsToUint(depth));
                }
                barrier();

                // Convert depth to view-space Z  (depth buffer is [0,1])
                float minDepthVS = uintBitsToFloat(s_minDepthInt);
                float maxDepthVS = uintBitsToFloat(s_maxDepthInt);

                // ---- build 4 side frustum planes in view space ----
                // Tile corners in NDC
                vec2 step  = 2.0 / vec2(tileCount);
                vec2 ndcLB = vec2(tileID) * step - 1.0;           // left-bottom
                vec2 ndcRT = ndcLB + step;                         // right-top

                // Unproject to view space on the near plane (z = -1 NDC → -near)
                mat4 invProj = inverse(uProjection);
                vec3 lb = vec3((invProj * vec4(ndcLB.x, ndcLB.y, -1.0, 1.0)).xyz);
                vec3 rb = vec3((invProj * vec4(ndcRT.x, ndcLB.y, -1.0, 1.0)).xyz);
                vec3 lt = vec3((invProj * vec4(ndcLB.x, ndcRT.y, -1.0, 1.0)).xyz);
                vec3 rt = vec3((invProj * vec4(ndcRT.x, ndcRT.y, -1.0, 1.0)).xyz);
                vec3 origin = vec3(0.0);

                vec4 planes[4];
                planes[0] = CreatePlane(origin, lb, lt); // left
                planes[1] = CreatePlane(origin, rt, rb); // right
                planes[2] = CreatePlane(origin, rb, lb); // bottom
                planes[3] = CreatePlane(origin, lt, rt); // top

                // ---- cull lights against this tile ----
                // Each thread handles a strided subset of lights
                for (int i = int(gl_LocalInvocationIndex); i < uLightCount; i += 256) {
                    vec4 lightViewPos = uView * vec4(lights[i].posRadius.xyz, 1.0);
                    float radius = lights[i].posRadius.w;

                    // Depth range test (view-space Z is negative in OpenGL)
                    float lightMinZ = -lightViewPos.z - radius;
                    float lightMaxZ = -lightViewPos.z + radius;
                    if (lightMaxZ < minDepthVS || lightMinZ > maxDepthVS) continue;

                    // Frustum plane test (sphere vs 4 planes)
                    bool inside = true;
                    for (int p = 0; p < 4; p++) {
                        float dist = dot(planes[p], vec4(lightViewPos.xyz, 1.0));
                        if (dist < -radius) { inside = false; break; }
                    }

                    if (inside) {
                        uint slot = atomicAdd(s_tileLightCount, 1u);
                        if (slot < 256u)
                            s_tileLightIndices[slot] = uint(i);
                    }
                }
                barrier();

                // ---- write results to SSBOs (one thread per tile) ----
                if (gl_LocalInvocationIndex == 0u) {
                    uint offset = uint(tileIndex) * 256u;
                    grid[tileIndex].offset = offset;
                    grid[tileIndex].count  = min(s_tileLightCount, 256u);
                    for (uint j = 0u; j < grid[tileIndex].count; j++)
                        lightIndices[offset + j] = s_tileLightIndices[j];
                }
            }
        )GLSL";

        m_lightCullShader = LinkProgram({
            CompileStage(GL_COMPUTE_SHADER, comp)
            });
    }

    void ShadowPass(EntityManager& em, auto& meshQuery)
    {
        std::vector<GPUShadowData> shadowDataVec;

        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
        glViewport(0, 0, m_shadowRes, m_shadowRes);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 4.0f);

        glUseProgram(m_shadowShader);

        int shadowIdx = 0;
        auto lightQuery = em.CreateQuery<PointLightComponent, Transform>();

        for (auto [entity, light, xform] : lightQuery)
        {
            if (shadowIdx >= MAX_SHADOW_LIGHTS) break;

            glm::vec3 pos = xform->getPosition();
            float     farPlane = light->radius;

            // Proyección igual para las 6 caras
            glm::mat4 shadowProj = glm::perspective(
                glm::radians(90.0f), 1.0f, 0.01f, farPlane);

            // Las 6 vistas del cubemap
            glm::mat4 views[6] = {
                shadowProj * glm::lookAt(pos, pos + glm::vec3(1, 0, 0), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0,-1, 0), glm::vec3(0, 0,-1)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 0, 1), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 0,-1), glm::vec3(0,-1, 0)),
            };

            // Subir las 6 matrices al geometry shader
            for (int f = 0; f < 6; f++) {
                std::string uni = "uLightSpaceMatrices[" + std::to_string(f) + "]";
                glUniformMatrix4fv(glGetUniformLocation(m_shadowShader, uni.c_str()),
                    1, GL_FALSE, glm::value_ptr(views[f]));
            }
            glUniform3fv(glGetUniformLocation(m_shadowShader, "uLightPos"),
                1, glm::value_ptr(pos));
            glUniform1f(glGetUniformLocation(m_shadowShader, "uFarPlane"), farPlane);
            glUniform1i(glGetUniformLocation(m_shadowShader, "uCubeArrayLayer"),
                shadowIdx * 6);

            // Renderizar todos los meshes para esta luz
            for (auto [me, meshC, xf] : meshQuery) {
                if (!meshC->enabled || !meshC->mesh) continue;
                glUniformMatrix4fv(glGetUniformLocation(m_shadowShader, "uModel"),
                    1, GL_FALSE, glm::value_ptr(xf->getModelMatrix()));
                meshC->mesh->drawDepthOnly(glm::mat4(1.0f), m_shadowShader); // MVP lo hace el geom shader
            }

            // Guardar metadatos
            GPUShadowData sd;
            for (int f = 0; f < 6; f++) sd.lightSpaceMatrices[f] = views[f];
            sd.lightIndex = shadowIdx;
            sd.farPlane = farPlane;
            shadowDataVec.push_back(sd);
            shadowIdx++;
        }

        m_shadowCount = shadowIdx;

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowDataSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUShadowData) * m_shadowCount, shadowDataVec.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_screenW, m_screenH);
    }

    // =====================================================
    //  Per-frame passes
    // =====================================================

    // --- Pass 1: stream PointLightComponent data to SSBO ---
    void UploadLights(EntityManager& em)
    {
        std::vector<GPUPointLight> lights;
        lights.reserve(MAX_LIGHTS);

        auto q = em.CreateQuery<PointLightComponent, Transform>();
        for (auto [entity, light, xform] : q) {
            if (lights.size() >= (size_t)MAX_LIGHTS) break;
            GPUPointLight gl;
            gl.posRadius = glm::vec4(xform->getPosition(), light->radius);
            gl.colorIntensity = glm::vec4(light->color, light->intensity);
            lights.push_back(gl);
        }

        m_lightCount = (int)lights.size();

        //Debug::Info("RenderSystem") << "Uploading " << m_lightCount << " lights\n";

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUPointLight) * m_lightCount,
            lights.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // --- Pass 2: render depth only into m_depthFBO ---
    void DepthPrePass(auto& meshQuery,
        const glm::mat4& view,
        const glm::mat4& projection)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
        glClear(GL_DEPTH_BUFFER_BIT);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_DEPTH_TEST);

        glUseProgram(m_depthShader);

        for (auto [entity, meshC, transform] : meshQuery) {
            if (!meshC->enabled) continue;
            Mesh* mesh = meshC->mesh.get();
            if (!mesh) continue;

            glm::mat4 mvp = projection * view * transform->getModelMatrix();
            mesh->drawDepthOnly(mvp, m_depthShader);
        }

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // --- Pass 3: dispatch compute shader to cull lights per tile ---
    void LightCullPass(const glm::mat4& view, const glm::mat4& projection)
    {
        glUseProgram(m_lightCullShader);

        // Depth texture — compute shader samples it to get min/max per tile
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glUniform1i(glGetUniformLocation(m_lightCullShader, "uDepthMap"), 0);

        glUniformMatrix4fv(glGetUniformLocation(m_lightCullShader, "uProjection"),
            1, GL_FALSE, glm::value_ptr(projection));
        glUniformMatrix4fv(glGetUniformLocation(m_lightCullShader, "uView"),
            1, GL_FALSE, glm::value_ptr(view));
        glUniform1i(glGetUniformLocation(m_lightCullShader, "uLightCount"),
            m_lightCount);
        glUniform2i(glGetUniformLocation(m_lightCullShader, "uScreenSize"),
            m_screenW, m_screenH);

        // Bind SSBOs for the compute shader
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_lightIndexSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_tileGridSSBO);

        // One workgroup per tile — each workgroup is 16×16 threads
        glDispatchCompute((GLuint)m_tilesX, (GLuint)m_tilesY, 1);

        // Ensure SSBO writes are visible to the fragment stage
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    // --- Pass 4: full shading — every frag reads from the tile SSBOs ---
    void ShadingPass(auto& meshQuery, const glm::mat4& view,
        const glm::mat4& projection, const glm::vec3& cameraPos)
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_lightIndexSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_tileGridSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_shadowDataSSBO); // NUEVO

        // Bind cube array al texture unit 1
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);     // NUEVO
        

        for (auto [entity, meshC, transform] : meshQuery) {
            if (!meshC->enabled || !meshC->mesh) continue;

            glm::mat4 model = transform->getModelMatrix();
            meshC->mesh->bindMaterial(model, view, projection);

            if (Material* mat = meshC->mesh->getMaterial()) {
                mat->setIVec2("uScreenSize", glm::ivec2(m_screenW, m_screenH));
                mat->setVec3("uCameraPos", cameraPos);
                mat->setInt("uShadowCubeArray", 5);   // NUEVO
                mat->setInt("uShadowCount", m_shadowCount); // NUEVO
            }
            meshC->mesh->draw();
        }
    }
};

#endif // RENDER_SYSTEM_HPP