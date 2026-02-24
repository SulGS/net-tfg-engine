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
        CompileDepthShader();
        CompileLightCullShader();
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

        // Recreate size-dependent resources
        glDeleteFramebuffers(1, &m_depthFBO);
        glDeleteTextures(1, &m_depthTex);
        glDeleteBuffers(1, &m_lightIndexSSBO);
        glDeleteBuffers(1, &m_tileGridSSBO);

        InitDepthFBO();
        InitSSBOs();
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
    }

private:
    // =====================================================
    //  Constants
    // =====================================================
    static constexpr int     TILE_SIZE = 16;
    static constexpr int     MAX_LIGHTS = 1024;
    static constexpr int     MAX_LIGHTS_TILE = 256; // per tile

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
    void ShadingPass(auto& meshQuery,
        const glm::mat4& view,
        const glm::mat4& projection,
        const glm::vec3& cameraPos)
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_lightIndexSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_tileGridSSBO);

        for (auto [entity, meshC, transform] : meshQuery) {
            if (!meshC->enabled) continue;
            Mesh* mesh = meshC->mesh.get();
            if (!mesh) continue;

            glm::mat4 model = transform->getModelMatrix();

            // 1. Bind shader + upload model/view/projection
            mesh->bindMaterial(model, view, projection);

            // 2. Now shader is bound — push Forward+ uniforms
            if (Material* mat = mesh->getMaterial()) {
                mat->setIVec2("uScreenSize", glm::ivec2(m_screenW, m_screenH));
                mat->setVec3("uCameraPos", cameraPos);
            }

            // 3. Draw with all uniforms already uploaded
            mesh->draw();
        }
    }
};

#endif // RENDER_SYSTEM_HPP