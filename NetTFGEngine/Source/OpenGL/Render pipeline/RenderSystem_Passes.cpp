#include "RenderSystem.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdint>
#include <cstdlib>

// FSR 1 SDK, CPU side: only for FsrEasuCon/FsrRcasCon (the shader constants). Its many unused static helpers would
// otherwise warn.
#if defined(_MSC_VER)
    #pragma warning(push, 0)
#elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define A_CPU 1
#include "FSR_SDK/ffx_a.h"
#include "FSR_SDK/ffx_fsr1.h"
#undef A_CPU
#if defined(_MSC_VER)
    #pragma warning(pop)
#elif defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif

// NIS SDK, CPU side: NISConfig / NVScalerUpdateConfig (the shader constants).
#include "NIS_SDK/NIS_Config.h"

void RenderSystem::GBufferPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
    const glm::mat4& view, const glm::mat4& projection)
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);
    glViewport(0, 0, m_screenW, m_screenH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    const GLint mvpLoc = glGetUniformLocation(m_gbufferShader, "uModelViewProjection");
    const GLint prevMvpLoc = glGetUniformLocation(m_gbufferShader, "uPrevModelViewProjection");
    const GLint normalMatLoc = glGetUniformLocation(m_gbufferShader, "uViewNormalMatrix");
    const glm::mat4 viewProjection = projection * view;
    bool engineShaderBound = false;

    // Last frame's model per entity, for the object velocity motion blur needs. Rebuilt every frame so destroyed
    // entities drop out; a new (or reused-id) entity starts with no motion.
    m_currModels.clear();

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh || meshC->additive) continue;

        glm::mat4 model = transform->getModelMatrix();
        m_currModels[entity] = model;

        glm::mat4 prevModel = model;
        if (auto it = m_prevModels.find(entity); it != m_prevModels.end()) {
            // A jump this big between two frames is a teleport (respawn, reused entity id), not motion to blur.
            const float kTeleportDistance = 5.0f;
            if (glm::length(glm::vec3(model[3]) - glm::vec3(it->second[3])) < kTeleportDistance)
                prevModel = it->second;
        }

        // Material with its own GBuffer variant (GBufferVariant.hpp): its vertex shader and surface evaluation,
        // so displacement / procedural normals / roughness reach SSAO and SSR. draw() binds its texture maps.
        Material* mat = meshC->mesh->getMaterial();
        if (mat && mat->hasGBufferProgram()) {
            mat->bindGBuffer(model, view, projection);
            meshC->mesh->draw();
            engineShaderBound = false;
            continue;
        }

        if (!engineShaderBound) {
            glUseProgram(m_gbufferShader);
            engineShaderBound = true;
        }

        // Per mesh on the CPU instead of an inverse() per vertex in the shader.
        const glm::mat4 modelView = view * model;
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelView)));
        glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(viewProjection * model));
        glUniformMatrix4fv(prevMvpLoc, 1, GL_FALSE, glm::value_ptr(viewProjection * prevModel));
        glUniformMatrix3fv(normalMatLoc, 1, GL_FALSE, glm::value_ptr(normalMatrix));

        meshC->mesh->drawGBuffer(m_gbufferShader);
    }

    m_prevModels.swap(m_currModels);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Reduced-res view-space Z for SSAO/SSR/motion blur (see the linear depth shader).
void RenderSystem::LinearDepthPass(const glm::mat4& projection, GLuint fbo, int w, int h, int scale)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_linearDepthShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepthTex);
    glUniform1i(glGetUniformLocation(m_linearDepthShader, "uDepthTex"), 0);
    glUniform1i(glGetUniformLocation(m_linearDepthShader, "uScale"), scale);
    glUniformMatrix4fv(glGetUniformLocation(m_linearDepthShader, "uInvProjection"),
        1, GL_FALSE, glm::value_ptr(glm::inverse(projection)));

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
    glEnable(GL_DEPTH_TEST);
}

// Reduced-res raw AO into m_ssaoTex, then the depth-aware blur into m_ssaoBlurTex (what ShadingPass samples, bilinear).
// Disabled: the blurred target is just cleared to 1.0 so the materials' multiply becomes a no-op.
void RenderSystem::ClearSSAO()
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
    glViewport(0, 0, m_ssaoW, m_ssaoH);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
    glEnable(GL_DEPTH_TEST);
}

void RenderSystem::SSAOPass(const glm::mat4& projection)
{
    const auto& rs = RenderSettings::instance();

    glViewport(0, 0, m_ssaoW, m_ssaoH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    if (!rs.getSSAOEnabled()) {
        ClearSSAO();
        return;
    }

    const glm::mat4 invProjection = glm::inverse(projection);
    glBindVertexArray(m_quadVAO);

    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
    glUseProgram(m_ssaoShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepthTex);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "uDepthTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_gbufferNormalTex);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "uNormalTex"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_linearDepthTex);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "uLinearDepthTex"), 2);
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "uInvProjection"),
        1, GL_FALSE, glm::value_ptr(invProjection));
    glUniform1i(glGetUniformLocation(m_ssaoShader, "uKernelSize"),
        std::min(rs.getSSAOSamples(), (int)m_ssaoKernel.size()));
    glUniform1f(glGetUniformLocation(m_ssaoShader, "uRadius"), rs.getSSAORadius());
    glUniform1f(glGetUniformLocation(m_ssaoShader, "uBias"), rs.getSSAOBias());
    glUniform1f(glGetUniformLocation(m_ssaoShader, "uIntensity"), rs.getSSAOIntensity());
    glUniform1i(glGetUniformLocation(m_ssaoShader, "uScale"), m_ssaoScale);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
    glUseProgram(m_ssaoBlurShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssaoTex);
    glUniform1i(glGetUniformLocation(m_ssaoBlurShader, "uAOTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_linearDepthTex);
    glUniform1i(glGetUniformLocation(m_ssaoBlurShader, "uLinearDepthTex"), 1);
    glUniform1f(glGetUniformLocation(m_ssaoBlurShader, "uRadius"), rs.getSSAORadius());
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
    glEnable(GL_DEPTH_TEST);
}

void RenderSystem::ResolveMSAA()
{
    if (m_msaaSamples <= 1)
        return;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_hdrFBO);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, m_screenW, m_screenH,
        0, 0, m_screenW, m_screenH,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBlitFramebuffer(0, 0, m_screenW, m_screenH,
        0, 0, m_screenW, m_screenH,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    glDrawBuffers(1, &drawBuf);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Uploads point lights to the light SSBO and the directional light (if any) to the dir light UBO.
void RenderSystem::CollectLightsPass(EntityManager& em)
{
    // Point lights
    std::vector<GPUPointLight> lightVec;
    lightVec.reserve(MAX_LIGHTS);

    auto lightQuery = em.CreateQuery<PointLightComponent, Transform>();
    for (auto [entity, light, xform] : lightQuery) {
        if ((int)lightVec.size() >= MAX_LIGHTS) break;
        GPUPointLight gl;
        gl.posRadius = glm::vec4(xform->getPosition(), light->radius);
        gl.colorIntensity = glm::vec4(light->color, light->intensity);
        lightVec.push_back(gl);
        // Note: castShadows is checked in ShadowPass when building the shadow
        // data SSBO — lights are still sent to the shading pass regardless.
    }

    m_lightCount = (int)lightVec.size();

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
    if (m_lightCount > 0)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUPointLight) * m_lightCount, lightVec.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Directional light — write into the CPU cache — no glGetBufferSubData readback stall.
    m_cpuDirLight = GPUDirLight{};  // clear each frame

    auto dirQuery = em.CreateQuery<DirectionalLightComponent>();
    bool foundDir = false;
    for (auto [entity, dirLight] : dirQuery) {
        if (foundDir) break;
        foundDir = true;

        glm::vec3 dir = glm::normalize(dirLight->direction);
        m_cpuDirLight.directionIntensity = glm::vec4(dir, dirLight->intensity);
        m_cpuDirLight.colorEnabled = glm::vec4(dirLight->color, 1.0f);
        // lightSpaceMatrix filled in DirShadowPass; pre-fill identity so the
        // shader can read it safely even when shadows are disabled.
        m_cpuDirLight.lightSpaceMatrix = glm::mat4(1.0f);
    }
    // If no dir light found, colorEnabled.a stays 0 → shader skips the term.

    glBindBuffer(GL_UNIFORM_BUFFER, m_dirLightUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GPUDirLight), &m_cpuDirLight);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

// Point light cubemap array; only lights with castShadows == true consume a shadow slot.
void RenderSystem::ShadowPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery)
{
    const auto& rs = RenderSettings::instance();

    std::vector<GPUShadowData> shadowDataVec;

    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
    glViewport(0, 0, m_shadowRes, m_shadowRes);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(rs.getShadowBiasFactor(), rs.getShadowBiasUnits());

    glUseProgram(m_shadowShader);

    int shadowIdx = 0;
    int lightBufIdx = 0; // mirrors insertion order in CollectLightsPass

    auto lightQuery = em.CreateQuery<PointLightComponent, Transform>();
    for (auto [entity, light, xform] : lightQuery) {
        if (lightBufIdx >= MAX_LIGHTS) break;

        // Skip lights that opt out of shadow casting.
        if (!light->castShadows) {
            lightBufIdx++;
            continue;
        }

        if (shadowIdx < MAX_SHADOW_LIGHTS) {
            glClear(GL_DEPTH_BUFFER_BIT);

            glm::vec3 pos = xform->getPosition();
            float     farPlane = light->radius;

            glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), 1.0f,
                rs.getShadowNearPlane(), farPlane);

            glm::mat4 views[6] = {
                shadowProj * glm::lookAt(pos, pos + glm::vec3(1, 0, 0), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0,-1, 0), glm::vec3(0, 0,-1)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 0, 1), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 0,-1), glm::vec3(0,-1, 0)),
            };

            for (int f = 0; f < 6; f++) {
                std::string uni = "uLightSpaceMatrices[" + std::to_string(f) + "]";
                glUniformMatrix4fv(glGetUniformLocation(m_shadowShader, uni.c_str()),
                    1, GL_FALSE, glm::value_ptr(views[f]));
            }
            glUniform3fv(glGetUniformLocation(m_shadowShader, "uLightPos"),
                1, glm::value_ptr(pos));
            glUniform1f(glGetUniformLocation(m_shadowShader, "uFarPlane"), farPlane);
            glUniform1i(glGetUniformLocation(m_shadowShader, "uCubeArrayLayer"), shadowIdx * 6);

            for (auto [me, meshC, xf] : meshQuery) {
                if (!meshC->enabled || !meshC->mesh || !meshC->castShadows || meshC->additive) continue;
                glUniformMatrix4fv(glGetUniformLocation(m_shadowShader, "uModel"),
                    1, GL_FALSE, glm::value_ptr(xf->getModelMatrix()));
                meshC->mesh->drawDepthOnly(glm::mat4(1.0f), m_shadowShader);
            }

            GPUShadowData sd;
            for (int f = 0; f < 6; f++) sd.lightSpaceMatrices[f] = views[f];
            sd.lightIndex = lightBufIdx;
            sd.farPlane = farPlane;
            shadowDataVec.push_back(sd);
            shadowIdx++;
        }

        lightBufIdx++;
    }

    m_shadowCount = shadowIdx;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowDataSSBO);
    if (m_shadowCount > 0)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUShadowData) * m_shadowCount, shadowDataVec.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
}

// Renders the scene from the directional light into a 2-D ortho shadow map, frustum centred on cameraPos. No glPolygonOffset (huge depth range would cause peter panning) — instead GL_DEPTH_CLAMP plus a receiver-side normal-scaled bias in the shading shader.
void RenderSystem::DirShadowPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
    const glm::vec3& cameraPos)
{
    // No dir light → nothing to do. Read from CPU cache (no GPU readback stall).
    if (m_cpuDirLight.colorEnabled.a < 0.5f)
        return;

    const auto& rs = RenderSettings::instance();
    const int   res = rs.getDirShadowResolution();
    const float kExtent = rs.getDirShadowExtent();
    const float kFar = rs.getDirShadowFar();

    glm::vec3 lightDir = glm::normalize(glm::vec3(m_cpuDirLight.directionIntensity));

    glm::vec3 up = (glm::abs(lightDir.y) > 0.99f)
        ? glm::vec3(1, 0, 0)
        : glm::vec3(0, 1, 0);

    // Eye pulled back kFar units from cameraPos; depth range [0, kFar*2] puts cameraPos at the midpoint so objects both in front of and behind it are captured.
    glm::mat4 lightView = glm::lookAt(
        cameraPos - lightDir * kFar,
        cameraPos,
        up);

    glm::mat4 lightProj = glm::ortho(
        -kExtent, kExtent,
        -kExtent, kExtent,
        0.0f, kFar * 2.0f);

    glm::mat4 lightSpace = lightProj * lightView;

    // Patch the computed matrix back into the CPU cache and re-upload the UBO
    // so ShadingPass can read it without a separate uniform.
    m_cpuDirLight.lightSpaceMatrix = lightSpace;
    glBindBuffer(GL_UNIFORM_BUFFER, m_dirLightUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GPUDirLight), &m_cpuDirLight);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, m_dirShadowFBO);
    glViewport(0, 0, res, res);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // GL_DEPTH_CLAMP prevents steep/back-facing casters from being clipped by the near plane; no glPolygonOffset here (bias is applied receiver-side instead).
    glEnable(GL_DEPTH_CLAMP);

    glUseProgram(m_dirShadowShader);
    glUniformMatrix4fv(glGetUniformLocation(m_dirShadowShader, "uLightSpaceMatrix"),
        1, GL_FALSE, glm::value_ptr(lightSpace));

    for (auto [entity, meshC, xf] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh || !meshC->castShadows || meshC->additive) continue;
        glUniformMatrix4fv(glGetUniformLocation(m_dirShadowShader, "uModel"),
            1, GL_FALSE, glm::value_ptr(xf->getModelMatrix()));
        meshC->mesh->drawDepthOnly(glm::mat4(1.0f), m_dirShadowShader);
    }

    glDisable(GL_DEPTH_CLAMP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
}

void RenderSystem::ShadingPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& cameraPos)
{
    const auto& rs = RenderSettings::instance();
    const GLuint targetFBO = (m_msaaSamples > 1) ? m_msaaFBO : m_hdrFBO;
    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuf);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    // Point light SSBO + shadow SSBO
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_shadowDataSSBO);

    // Directional light UBO
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_dirLightUBO);

    // Point light shadow cubemap array — texture unit 5
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);

    // Directional light shadow map — texture unit 6
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, m_dirShadowTex);

    // Screen-space AO (all 1.0 when SSAO is off) — texture unit 7
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, m_ssaoBlurTex);

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh || meshC->additive) continue; // additive ones: see AdditivePass

        // Pipeline state offered to every material, set only if the shader has it. Set BEFORE bind: Material::bind()
        // uploads them, so afterwards they'd only reach the GPU next frame.
        if (Material* mat = meshC->mesh->getMaterial()) {
            mat->setVec3IfPresent("uCameraPos", cameraPos);
            mat->setIntIfPresent("uShadowCubeArray", 5);
            mat->setIntIfPresent("uDirShadowMap", 6);
            mat->setIntIfPresent("uSSAOTex", 7);
            mat->setIntIfPresent("uSSAOScale", m_ssaoScale);
            mat->setIntIfPresent("uShadowCount", m_shadowCount);
            mat->setIntIfPresent("uLightCount", m_lightCount);
            mat->setIntIfPresent("uShadowRes", m_shadowRes);
            mat->setIntIfPresent("uDirShadowRes", rs.getDirShadowResolution());
        }

        glm::mat4 model = transform->getModelMatrix();
        meshC->mesh->bindMaterial(model, view, projection);
        meshC->mesh->draw();
    }

    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ResolveMSAA();

    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glViewport(0, 0, m_screenW, m_screenH);
}

// Light-only meshes (MeshComponent::additive) summed on the shaded scene after ShadingPass + ResolveMSAA: depth-tested
// against resolved depth, no depth write, (SRC_ALPHA, ONE) so order doesn't matter; stays HDR for bloom.
// Uses drawGeometryOnly(): these unlit shaders need no texture units, so Mesh::draw()'s sampler uniforms are skipped.
void RenderSystem::AdditivePass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& cameraPos)
{
    bool any = false;
    for (auto [entity, meshC, transform] : meshQuery) {
        if (meshC->enabled && meshC->mesh && meshC->additive) { any = true; break; }
    }
    if (!any) return;

    // Blend state isn't guaranteed on entry (ParticleSystem::Draw disables
    // it when done), so set what we need and put back what we found.
    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    GLint srcRGB, dstRGB, srcA, dstA;
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);

    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuf);
    glViewport(0, 0, m_screenW, m_screenH);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh || !meshC->additive) continue;

        // Set before bind: Material::bind() is what uploads the uniform map. Optional:
        // an additive shader that never looks at the camera is fine too.
        if (Material* mat = meshC->mesh->getMaterial())
            mat->setVec3IfPresent("uCameraPos", cameraPos);

        meshC->mesh->bindMaterial(transform->getModelMatrix(), view, projection);
        meshC->mesh->drawGeometryOnly();
    }

    glDepthMask(GL_TRUE);
    glBlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
    if (!blendWasOn) glDisable(GL_BLEND);
}

// SSR at its resolution scale: sanitised reduced-res scene copy (+ mips for rough surfaces), trace into m_ssrTraceTex,
// then a full-res depth-aware composite onto m_hdrFBO. Leaves m_hdrFBO bound with depth test on, as AdditivePass does, for the
// particles that follow.
void RenderSystem::SSRPass(const glm::mat4& projection)
{
    const auto& rs = RenderSettings::instance();

    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    GLint srcRGB, dstRGB, srcA, dstA;
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);

    const glm::mat4 invProjection = glm::inverse(projection);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glBindVertexArray(m_quadVAO);
    glViewport(0, 0, m_ssrW, m_ssrH);

    // 1. Sanitised reduced-res copy of the scene (see the copy shader), then its mip chain.
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssrSceneFBO);
    glUseProgram(m_ssrCopyShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
    glUniform1i(glGetUniformLocation(m_ssrCopyShader, "uHDRBuffer"), 0);
    // Well past tonemap white (~11): reflections of emissive stuff still bloom, without one pixel dominating a mip.
    glUniform1f(glGetUniformLocation(m_ssrCopyShader, "uMaxLuminance"), 64.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindTexture(GL_TEXTURE_2D, m_ssrSceneTex);
    glGenerateMipmap(GL_TEXTURE_2D);

    // 2. Trace at the SSR resolution. Cleared first: pixels without a hit discard and must read as "no reflection".
    // Alpha must be 0 too (ShadingPass leaves the clear colour at alpha 1): alpha 1 would make the composite's
    // ONE_MINUS_SRC_ALPHA wipe the scene to black wherever there is no reflection.
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssrTraceFBO);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(m_ssrShader);

    // Unit 0 already holds the scene copy.
    glUniform1i(glGetUniformLocation(m_ssrShader, "uSceneTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepthTex);
    glUniform1i(glGetUniformLocation(m_ssrShader, "uDepthTex"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_gbufferNormalTex);
    glUniform1i(glGetUniformLocation(m_ssrShader, "uNormalTex"), 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_gbufferMaterialTex);
    glUniform1i(glGetUniformLocation(m_ssrShader, "uMaterialTex"), 3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_gbufferAlbedoTex);
    glUniform1i(glGetUniformLocation(m_ssrShader, "uAlbedoTex"), 4);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, SSRLinearDepthTex());
    glUniform1i(glGetUniformLocation(m_ssrShader, "uLinearDepthTex"), 5);
    glUniform1i(glGetUniformLocation(m_ssrShader, "uScale"), m_ssrScale);

    glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uInvProjection"),
        1, GL_FALSE, glm::value_ptr(invProjection));
    glUniform1i(glGetUniformLocation(m_ssrShader, "uSteps"), rs.getSSRSteps());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uMaxDistance"), rs.getSSRMaxDistance());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uThickness"), rs.getSSRThickness());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uMaxRoughness"), rs.getSSRMaxRoughness());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uIntensity"), rs.getSSRIntensity());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uMaxLod"), (float)(m_ssrSceneMips - 1));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 3. Full-res composite. Premultiplied: plain add when alpha is 0, partial replace otherwise (see the shaders).
    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuf);
    glViewport(0, 0, m_screenW, m_screenH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_ssrCompositeShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssrTraceTex);
    glUniform1i(glGetUniformLocation(m_ssrCompositeShader, "uTraceTex"), 0);
    glUniform1i(glGetUniformLocation(m_ssrCompositeShader, "uLinearDepthTex"), 5);
    glUniform1i(glGetUniformLocation(m_ssrCompositeShader, "uDepthTex"), 1);
    glUniform1i(glGetUniformLocation(m_ssrCompositeShader, "uScale"), m_ssrScale);
    glUniformMatrix4fv(glGetUniformLocation(m_ssrCompositeShader, "uInvProjection"),
        1, GL_FALSE, glm::value_ptr(invProjection));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glBlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
    if (!blendWasOn) glDisable(GL_BLEND);
}

// Velocity -> tile max -> neighbour max -> gather (see CompileMotionBlurShaders). Reads a copy of the HDR scene and
// writes the blurred result back into m_hdrFBO, only where something near moves (the gather discards elsewhere).
// Leaves m_hdrFBO bound with depth test on.
void RenderSystem::MotionBlurPass(const glm::mat4& viewProjection, float deltaTime)
{
    const auto& rs = RenderSettings::instance();
    // First frame: no previous camera to compare with. Strength 0: nothing to blur.
    if (!m_hasPrevViewProjection || rs.getMotionBlurStrength() <= 0.0f)
        return;

    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glBindVertexArray(m_quadVAO);

    // Shutter open for strength x a 60 FPS frame; expressed relative to this frame so the blur length is the same
    // at any frame rate.
    const float shutterSeconds = rs.getMotionBlurStrength() / 60.0f;
    const float exposureScale = shutterSeconds / std::max(deltaTime, 1e-4f);

    // 1. Velocity (camera + object), as a clamped blur radius in pixels.
    glBindFramebuffer(GL_FRAMEBUFFER, m_mbVelocityFBO);
    glViewport(0, 0, m_screenW, m_screenH);
    glUseProgram(m_mbVelocityShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepthTex);
    glUniform1i(glGetUniformLocation(m_mbVelocityShader, "uDepthTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_gbufferVelocityTex);
    glUniform1i(glGetUniformLocation(m_mbVelocityShader, "uObjectVelocityTex"), 1);
    glUniformMatrix4fv(glGetUniformLocation(m_mbVelocityShader, "uInvViewProjection"),
        1, GL_FALSE, glm::value_ptr(glm::inverse(viewProjection)));
    glUniformMatrix4fv(glGetUniformLocation(m_mbVelocityShader, "uPrevViewProjection"),
        1, GL_FALSE, glm::value_ptr(m_prevViewProjection));
    glUniform1f(glGetUniformLocation(m_mbVelocityShader, "uExposureScale"), exposureScale);
    glUniform1f(glGetUniformLocation(m_mbVelocityShader, "uMaxRadius"), (float)m_mbTile);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 2. Largest velocity per tile.
    glBindFramebuffer(GL_FRAMEBUFFER, m_mbTileMaxFBO);
    glViewport(0, 0, m_mbTilesW, m_mbTilesH);
    glUseProgram(m_mbTileMaxShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_mbVelocityTex);
    glUniform1i(glGetUniformLocation(m_mbTileMaxShader, "uVelocityTex"), 0);
    glUniform1i(glGetUniformLocation(m_mbTileMaxShader, "uTile"), m_mbTile);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 3. ...and over each tile's 3x3 neighbourhood: any blur that can reach a pixel starts at most one tile away.
    glBindFramebuffer(GL_FRAMEBUFFER, m_mbNeighborMaxFBO);
    glUseProgram(m_mbNeighborMaxShader);
    glBindTexture(GL_TEXTURE_2D, m_mbTileMaxTex);
    glUniform1i(glGetUniformLocation(m_mbNeighborMaxShader, "uTileMaxTex"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 4. Source copy of the scene: the gather reads it while writing into m_hdrFBO.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_hdrFBO);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_mbSourceFBO);
    glBlitFramebuffer(0, 0, m_screenW, m_screenH,
        0, 0, m_screenW, m_screenH,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // 5. Gather.
    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuf);
    glViewport(0, 0, m_screenW, m_screenH);
    glUseProgram(m_mbGatherShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_mbSourceTex);
    glUniform1i(glGetUniformLocation(m_mbGatherShader, "uSourceTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_mbVelocityTex);
    glUniform1i(glGetUniformLocation(m_mbGatherShader, "uVelocityTex"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_mbNeighborMaxTex);
    glUniform1i(glGetUniformLocation(m_mbGatherShader, "uNeighborMaxTex"), 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_linearDepthTex);
    glUniform1i(glGetUniformLocation(m_mbGatherShader, "uLinearDepthTex"), 3);
    glUniform1i(glGetUniformLocation(m_mbGatherShader, "uDepthScale"), m_ssaoScale);
    glUniform1i(glGetUniformLocation(m_mbGatherShader, "uTile"), m_mbTile);
    glUniform1i(glGetUniformLocation(m_mbGatherShader, "uSamples"), rs.getMotionBlurSamples());
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    if (blendWasOn) glEnable(GL_BLEND);
}

void RenderSystem::TonemapPass()
{
    const auto& rs = RenderSettings::instance();

    glBindFramebuffer(GL_FRAMEBUFFER, m_ldrFBO);
    glViewport(0, 0, m_screenW, m_screenH);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_tonemapShader);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uHDRBuffer"), 0);

    const bool bloomOn = rs.getBloomEnabled();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloomOn ? m_bloomPingTex : 0);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uBloomTex"), 1);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uBloomEnabled"), bloomOn ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uBloomStrength"), rs.getBloomStrength());

    glUniform1f(glGetUniformLocation(m_tonemapShader, "uExposure"), rs.getExposure());
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uFilmicEnabled"), rs.getFilmicEnabled() ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uGamma"), rs.getGamma());

    glUniform1f(glGetUniformLocation(m_tonemapShader, "uA"), rs.getFilmicShoulder());
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uB"), rs.getFilmicLinearStrength());
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uC"), rs.getFilmicLinearAngle());
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uD"), rs.getFilmicToeStrength());
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uE"), rs.getFilmicToeNumerator());
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uF"), rs.getFilmicToeDenominator());
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uW"), rs.getFilmicLinearWhite());

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_DEPTH_TEST);
}

void RenderSystem::BloomPass()
{
    const auto& rs = RenderSettings::instance();
    const int   bW = std::max(1, m_screenW / 2);
    const int   bH = std::max(1, m_screenH / 2);

    glDisable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, m_bloomThreshFBO);
    glViewport(0, 0, m_screenW, m_screenH);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(m_bloomThreshShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
    glUniform1i(glGetUniformLocation(m_bloomThreshShader, "uHDRBuffer"), 0);
    glUniform1f(glGetUniformLocation(m_bloomThreshShader, "uThreshold"), rs.getBloomThreshold());
    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, m_bloomPingFBO);
    glViewport(0, 0, bW, bH);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(m_bloomKawaseShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bloomThreshTex);
    glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uTex"), 0);
    glUniform2f(glGetUniformLocation(m_bloomKawaseShader, "uTexelSize"),
        1.0f / float(m_screenW), 1.0f / float(m_screenH));
    glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uIteration"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    const int passes = rs.getBloomPasses();
    GLuint src = m_bloomPingTex;
    GLuint dstFBO = m_bloomPongFBO;

    for (int i = 1; i <= passes; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, dstFBO);
        glViewport(0, 0, bW, bH);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glUniform2f(glGetUniformLocation(m_bloomKawaseShader, "uTexelSize"),
            1.0f / float(bW), 1.0f / float(bH));
        glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uIteration"), i);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        if (dstFBO == m_bloomPongFBO) { src = m_bloomPongTex; dstFBO = m_bloomPingFBO; }
        else { src = m_bloomPingTex; dstFBO = m_bloomPongFBO; }
    }
    if (src != m_bloomPingTex) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomPingFBO);
        glViewport(0, 0, bW, bH);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_bloomPongTex);
        glUniform2f(glGetUniformLocation(m_bloomKawaseShader, "uTexelSize"),
            1.0f / float(bW), 1.0f / float(bH));
        glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uIteration"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_DEPTH_TEST);
    glViewport(0, 0, m_screenW, m_screenH);
}

// Draws the render-resolution LDR image to targetFBO at targetW x targetH. In Bilinear mode the target is the window at
// the output size, sampling with vUV (bilinear), so a render resolution different from the window's is scaled here for
// free. For Nearest/FSR it runs at render resolution into the pre-scale target (anti-aliasing has to happen before the
// scale). Texel sizes stay those of the render resolution, which is what the FXAA edge search is defined in. Leaves the
// viewport at the target size (the output size for the UI drawn afterwards when targetFBO is 0).
void RenderSystem::FXAAPass(GLuint targetFBO, int targetW, int targetH)
{
    const auto& rs = RenderSettings::instance();

    // No FXAA: a plain bilinear copy, exact at the same size (drawn, not blitted: see CompileCopyShader).
    if (!rs.getFXAAEnabled()) {
        CopyPass(m_ldrTex, targetFBO, targetW, targetH, false);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    glViewport(0, 0, targetW, targetH);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_fxaaShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ldrTex);
    glUniform1i(glGetUniformLocation(m_fxaaShader, "uLDRBuffer"), 0);
    glUniform2f(glGetUniformLocation(m_fxaaShader, "uTexelSize"),
        1.0f / float(m_screenW), 1.0f / float(m_screenH));
    glUniform1f(glGetUniformLocation(m_fxaaShader, "uSubpix"), rs.getFXAASubpix());
    glUniform1f(glGetUniformLocation(m_fxaaShader, "uEdgeThreshold"), rs.getFXAAEdgeThreshold());
    glUniform1f(glGetUniformLocation(m_fxaaShader, "uEdgeThresholdMin"), rs.getFXAAEdgeThresholdMin());

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
}

// At native resolution there is nothing to scale, so every mode is the plain Bilinear path (no extra targets). FSR
// only upscales (EASU is a 1x-4x area filter): at a higher render resolution it falls back to Bilinear; NIS too, and
// also past 2x per axis (NVScalerUpdateConfig rejects it); Nearest works both ways.
UpscaleMode RenderSystem::ActiveUpscaleMode() const
{
    if (m_screenW == m_outputW && m_screenH == m_outputH) return UpscaleMode::Bilinear;

    const bool upscaling = m_screenW <= m_outputW && m_screenH <= m_outputH;
    switch (RenderSettings::instance().getUpscaleMode()) {
    case UpscaleMode::Nearest:
        return UpscaleMode::Nearest;
    case UpscaleMode::FSR1:
        if (m_fsrEasuShader && m_fsrRcasShader && upscaling)
            return UpscaleMode::FSR1;
        return UpscaleMode::Bilinear;
    case UpscaleMode::NIS:
        if (m_nisShader && upscaling && m_screenW * 2 >= m_outputW && m_screenH * 2 >= m_outputH)
            return UpscaleMode::NIS;
        return UpscaleMode::Bilinear;
    default:
        return UpscaleMode::Bilinear;
    }
}

// Nearest: each target pixel takes the one source pixel it falls in, no filtering. Leaves targetFBO bound and the
// viewport at its size (the output size for the UI drawn afterwards when it is the window).
void RenderSystem::CopyPass(GLuint sourceTex, GLuint targetFBO, int targetW, int targetH, bool nearest)
{
    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    glViewport(0, 0, targetW, targetH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_copyShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTex);
    glUniform1i(glGetUniformLocation(m_copyShader, "uTex"), 0);
    glUniform1i(glGetUniformLocation(m_copyShader, "uNearest"), nearest ? 1 : 0);

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
}

// AMD FSR 1: EASU scales inputTex (render resolution, tonemapped and anti-aliased) to the output resolution, then
// RCAS sharpens it into the window. Leaves the viewport at the output size for the UI drawn afterwards.
void RenderSystem::FSRPass(GLuint inputTex)
{
    const auto& rs = RenderSettings::instance();

    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(m_quadVAO);

    // EASU
    AU1 con0[4], con1[4], con2[4], con3[4];
    FsrEasuCon(con0, con1, con2, con3,
        static_cast<AF1>(m_screenW), static_cast<AF1>(m_screenH),   // viewport inside the input: all of it
        static_cast<AF1>(m_screenW), static_cast<AF1>(m_screenH),   // input texture size
        static_cast<AF1>(m_outputW), static_cast<AF1>(m_outputH));  // output size

    glBindFramebuffer(GL_FRAMEBUFFER, m_upscaleOutFBO);
    glViewport(0, 0, m_outputW, m_outputH);
    glUseProgram(m_fsrEasuShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTex);
    glUniform1i(glGetUniformLocation(m_fsrEasuShader, "uInput"), 0);
    glUniform4uiv(glGetUniformLocation(m_fsrEasuShader, "uCon0"), 1, con0);
    glUniform4uiv(glGetUniformLocation(m_fsrEasuShader, "uCon1"), 1, con1);
    glUniform4uiv(glGetUniformLocation(m_fsrEasuShader, "uCon2"), 1, con2);
    glUniform4uiv(glGetUniformLocation(m_fsrEasuShader, "uCon3"), 1, con3);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // RCAS. The SDK's sharpness is in stops of reduction (0 = maximum); the setting is 0..1 with 1 = maximum, mapped
    // over 0..2 stops (2 stops is already very soft).
    AU1 rcasCon[4];
    FsrRcasCon(rcasCon, (1.0f - rs.getFSRSharpness()) * 2.0f);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_outputW, m_outputH);
    glUseProgram(m_fsrRcasShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_upscaleOutTex);
    glUniform1i(glGetUniformLocation(m_fsrRcasShader, "uInput"), 0);
    glUniform4uiv(glGetUniformLocation(m_fsrRcasShader, "uCon"), 1, rcasCon);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

// NVIDIA Image Scaling: one NVScaler dispatch scales and sharpens inputTex (render resolution, tonemapped and
// anti-aliased, display space as the SDK asks) into the upscale output target, then an exact copy puts it in the
// window. One work group per NIS_BLOCK_WIDTH x NIS_BLOCK_HEIGHT output block; stores past the edge are dropped by GL.
// Leaves the viewport at the output size for the UI drawn afterwards.
void RenderSystem::NISPass(GLuint inputTex)
{
    NISConfig config{};
    if (!NVScalerUpdateConfig(config, RenderSettings::instance().getNISSharpness(),
            0, 0, m_screenW, m_screenH, m_screenW, m_screenH,     // input viewport: all of it
            0, 0, m_outputW, m_outputH, m_outputW, m_outputH)) {  // output viewport: all of it
        // Out of NIS's 1x-2x range (ActiveUpscaleMode already filters it): plain bilinear instead.
        CopyPass(inputTex, 0, m_outputW, m_outputH, false);
        return;
    }

    glBindBuffer(GL_UNIFORM_BUFFER, m_nisConfigUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(NISConfig), &config);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_nisConfigUBO);

    glUseProgram(m_nisShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTex);
    glBindSampler(0, m_nisSampler);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_nisCoefScaleTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_nisCoefUsmTex);
    glBindImageTexture(0, m_upscaleOutTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

    glDispatchCompute((m_outputW + m_nisBlockW - 1) / m_nisBlockW, (m_outputH + m_nisBlockH - 1) / m_nisBlockH, 1);

    // The copy below samples what the image stores wrote.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindSampler(0, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    CopyPass(m_upscaleOutTex, 0, m_outputW, m_outputH, false);
}
