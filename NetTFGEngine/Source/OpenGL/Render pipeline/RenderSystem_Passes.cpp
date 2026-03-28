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
    // Use the first DirectionalLightComponent found; zero-out if none.
    GPUDirLight gpuDir{};

    auto dirQuery = em.CreateQuery<DirectionalLightComponent>();
    bool foundDir = false;
    for (auto [entity, dirLight] : dirQuery) {
        if (foundDir) break; // only one supported
        foundDir = true;

        glm::vec3 dir = glm::normalize(dirLight->direction);
        gpuDir.directionIntensity = glm::vec4(dir, dirLight->intensity);
        gpuDir.colorEnabled = glm::vec4(dirLight->color, 1.0f); // a=1 means enabled

        // lightSpaceMatrix is filled in DirShadowPass once we know shadows are
        // enabled.  We pre-fill identity here so the shader can at least read it
        // without undefined behaviour if shadows are disabled.
        gpuDir.lightSpaceMatrix = glm::mat4(1.0f);
    }
    // If no dir light found, colorEnabled.a stays 0 → shader skips the term.

    glBindBuffer(GL_UNIFORM_BUFFER, m_dirLightUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GPUDirLight), &gpuDir);
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
                if (!meshC->enabled || !meshC->mesh) continue;
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
//  The light-space matrix is also patched into the dir
//  light UBO so the shading pass can transform world
//  positions into shadow space without a second uniform.
//
//  Scene bounds: we use a fixed-size orthographic frustum
//  centred on the world origin.  For production use you
//  would compute a tight fit around the camera frustum
//  (CSM / stable shadow mapping), but this simple form is
//  correct and produces good results for bounded scenes.
// =====================================================
void RenderSystem::DirShadowPass(EntityManager::Query<MeshComponent, Transform>& meshQuery)
{
    // Read the current dir light UBO to check whether shadows are wanted.
    // We only need the colorEnabled.a flag — read it back from the buffer.
    GPUDirLight gpuDir{};
    glBindBuffer(GL_UNIFORM_BUFFER, m_dirLightUBO);
    glGetBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GPUDirLight), &gpuDir);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // a == 0 → no directional light, or castShadows == false at the ECS level.
    // We still need to know castShadows — it was encoded by CollectLightsPass:
    // if the component has castShadows=false we set a=1 but skip the shadow matrix
    // update.  Easier: re-query here.  (It's a cheap query, only one entity.)
    // Actually the cleanest approach: CollectLightsPass already wrote a=1 when
    // the light exists.  We check an additional flag via a re-read of the component.
    // To avoid coupling, we keep a small cached bool from CollectLightsPass.
    // Since we don't have that cache, we do a lightweight re-query.

    // If no dir light at all, nothing to do.
    if (gpuDir.colorEnabled.a < 0.5f)
        return;

    // Re-query to get castShadows.  This is the only place we need it after
    // CollectLightsPass, and the query is O(1) for a single component.
    // (If your ECS is expensive to query mid-frame, cache a bool in the class.)
    bool castShadows = false;
    glm::vec3 lightDir = glm::vec3(gpuDir.directionIntensity);

    // We need the EntityManager here — but DirShadowPass only receives the
    // mesh query.  Solution: the shadow pass reads castShadows from the UBO
    // extension.  For simplicity we always render the shadow map when the
    // light is present; the component's castShadows flag suppresses binding
    // in the shading pass instead (see ShadingPass).
    // *** If you want per-component castShadows to fully skip the render,
    //     pass EntityManager& em into this function and re-query here. ***
    castShadows = true; // render shadow map if light exists

    const auto& rs = RenderSettings::instance();
    const int   res = m_shadowRes;

    // Orthographic frustum dimensions read from RenderSettings so they can be
    // tuned per-quality-preset or adjusted at runtime without a recompile.
    const float kExtent = rs.getDirShadowExtent();
    const float kNear = rs.getDirShadowNear();
    const float kFar = rs.getDirShadowFar();

    glm::vec3 up = (glm::abs(lightDir.y) > 0.99f)
        ? glm::vec3(1, 0, 0)
        : glm::vec3(0, 1, 0);

    glm::mat4 lightView = glm::lookAt(
        -lightDir * 50.0f,   // eye: pull back from origin along the light direction
        glm::vec3(0.0f),     // look-at: world origin (centre of your scene)
        up);

    glm::mat4 lightProj = glm::ortho(
        -kExtent, kExtent,
        -kExtent, kExtent,
        kNear, kFar);

    glm::mat4 lightSpace = lightProj * lightView;

    // Patch the light-space matrix back into the UBO.
    gpuDir.lightSpaceMatrix = lightSpace;
    glBindBuffer(GL_UNIFORM_BUFFER, m_dirLightUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GPUDirLight), &gpuDir);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // ---- Render depth into m_dirShadowFBO ----
    glBindFramebuffer(GL_FRAMEBUFFER, m_dirShadowFBO);
    glViewport(0, 0, res, res);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(rs.getShadowBiasFactor(), rs.getShadowBiasUnits());

    glUseProgram(m_dirShadowShader);
    glUniformMatrix4fv(glGetUniformLocation(m_dirShadowShader, "uLightSpaceMatrix"),
        1, GL_FALSE, glm::value_ptr(lightSpace));

    for (auto [entity, meshC, xf] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh) continue;
        glUniformMatrix4fv(glGetUniformLocation(m_dirShadowShader, "uModel"),
            1, GL_FALSE, glm::value_ptr(xf->getModelMatrix()));
        meshC->mesh->drawDepthOnly(glm::mat4(1.0f), m_dirShadowShader);
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
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
    const GLuint targetFBO = (m_msaaSamples > 1) ? m_msaaFBO : m_hdrFBO;
    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuf);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

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
            mat->setInt("uDirShadowMap", 6);  // NEW
            mat->setInt("uShadowCount", m_shadowCount);
            mat->setInt("uLightCount", m_lightCount);
            mat->setInt("uShadowRes", m_shadowRes);
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