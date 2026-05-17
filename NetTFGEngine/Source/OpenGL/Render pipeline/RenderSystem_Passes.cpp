#include "RenderSystem.hpp"
#include <glm/gtc/matrix_transform.hpp>

// =====================================================
//  GBufferPass
// =====================================================
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

    glUseProgram(m_gbufferShader);

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh) continue;

        glm::mat4 model = transform->getModelMatrix();
        glUniformMatrix4fv(glGetUniformLocation(m_gbufferShader, "uModel"),
            1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix4fv(glGetUniformLocation(m_gbufferShader, "uView"),
            1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(glGetUniformLocation(m_gbufferShader, "uProjection"),
            1, GL_FALSE, glm::value_ptr(projection));

        meshC->mesh->drawGBuffer(m_gbufferShader);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// =====================================================
//  ResolveMSAA
// =====================================================
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

// =====================================================
//  CollectLightsPass
//  Uploads point lights (respecting castShadows) to the
//  light SSBO and uploads the directional light (if any)
//  to the dir light UBO.
// =====================================================
void RenderSystem::CollectLightsPass(EntityManager& em)
{
    // ---- Point lights ----
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

    // ---- Directional light ----
    // Write into the CPU cache — no glGetBufferSubData readback stall.
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

// =====================================================
//  ShadowPass  — point light cubemap array
//  Only lights with castShadows == true consume a shadow slot.
// =====================================================
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
                if (!meshC->enabled || !meshC->mesh || !meshC->castShadows) continue;
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

// =====================================================
//  DirShadowPass
//  Renders the scene from the directional light's point
//  of view into a 2-D orthographic shadow map.
//
//  The frustum is centred on cameraPos so that distant
//  objects visible to the camera always receive shadows.
//
//  Peter panning fix:
//    - glPolygonOffset is NOT used for directional shadows.
//      With kFar = 800 the depth range is huge, making any
//      fixed polygon offset massive in world space and
//      causing geometry to visually detach from its shadow.
//    - Instead we enable GL_DEPTH_CLAMP (prevents near-plane
//      clipping of steep casters) and rely on a receiver-side
//      normal-scaled bias in the shading shader, normalised by
//      kFar so it stays correct regardless of frustum depth.
// =====================================================
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

    // Eye pulled back kFar units upstream from cameraPos.
    // Depth range [0, kFar*2] places cameraPos at the midpoint so objects
    // up to kFar units in front of AND behind the camera are captured.
    // The old [kNear, kFar] with a large negative kNear wasted depth range
    // behind the eye and cut off distant forward objects.
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

    // ---- Depth-only render ----
    glBindFramebuffer(GL_FRAMEBUFFER, m_dirShadowFBO);
    glViewport(0, 0, res, res);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // GL_DEPTH_CLAMP prevents steep/back-facing casters from being clipped by
    // the near plane, which is the main cause of missing shadow fragments.
    // We do NOT use glPolygonOffset here — with kFar = 800 a fixed offset
    // translates to several world-units of displacement, causing peter panning.
    // Bias is applied receiver-side in the shading shader instead (see below).
    glEnable(GL_DEPTH_CLAMP);

    glUseProgram(m_dirShadowShader);
    glUniformMatrix4fv(glGetUniformLocation(m_dirShadowShader, "uLightSpaceMatrix"),
        1, GL_FALSE, glm::value_ptr(lightSpace));

    for (auto [entity, meshC, xf] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh || !meshC->castShadows) continue;
        glUniformMatrix4fv(glGetUniformLocation(m_dirShadowShader, "uModel"),
            1, GL_FALSE, glm::value_ptr(xf->getModelMatrix()));
        meshC->mesh->drawDepthOnly(glm::mat4(1.0f), m_dirShadowShader);
    }

    glDisable(GL_DEPTH_CLAMP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
}

// =====================================================
//  ShadingPass
// =====================================================
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

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh) continue;

        glm::mat4 model = transform->getModelMatrix();
        meshC->mesh->bindMaterial(model, view, projection);

        if (Material* mat = meshC->mesh->getMaterial()) {
            mat->setVec3("uCameraPos", cameraPos);
            mat->setInt("uShadowCubeArray", 5);
            mat->setInt("uDirShadowMap", 6);
            mat->setInt("uShadowCount", m_shadowCount);
            mat->setInt("uLightCount", m_lightCount);
            mat->setInt("uShadowRes", m_shadowRes);
            mat->setInt("uDirShadowRes", rs.getDirShadowResolution());
        }
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

// =====================================================
//  TonemapPass
// =====================================================
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

// =====================================================
//  BloomPass
// =====================================================
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

// =====================================================
//  FXAAPass
// =====================================================
void RenderSystem::FXAAPass()
{
    const auto& rs = RenderSettings::instance();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!rs.getFXAAEnabled()) {
        glUseProgram(m_bloomKawaseShader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_ldrTex);
        glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uTex"), 0);
        glUniform2f(glGetUniformLocation(m_bloomKawaseShader, "uTexelSize"),
            1.0f / float(m_screenW), 1.0f / float(m_screenH));
        glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uIteration"), 0);
        glBindVertexArray(m_quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
        return;
    }

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