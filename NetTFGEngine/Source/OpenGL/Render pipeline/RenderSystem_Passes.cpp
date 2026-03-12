#include "RenderSystem.hpp"

// =====================================================
//  ShadowPass
// =====================================================
void RenderSystem::ShadowPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery)
{
    const auto& rs = RenderSettings::instance();

    // Build light list and shadow data in a single pass over lights
    // so that sd.lightIndex stores the absolute light-buffer slot index,
    // matching what the fragment shader reads from uLightCount lights[].
    std::vector<GPUPointLight>  lightVec;
    std::vector<GPUShadowData>  shadowDataVec;
    lightVec.reserve(MAX_LIGHTS);

    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
    glViewport(0, 0, m_shadowRes, m_shadowRes);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(rs.getShadowBiasFactor(), rs.getShadowBiasUnits());

    glUseProgram(m_shadowShader);

    int  shadowIdx = 0;
    auto lightQuery = em.CreateQuery<PointLightComponent, Transform>();

    for (auto [entity, light, xform] : lightQuery) {
        if ((int)lightVec.size() >= MAX_LIGHTS) break;

        int lightBufIdx = (int)lightVec.size();

        // Accumulate into the light SSBO buffer
        GPUPointLight gl;
        gl.posRadius = glm::vec4(xform->getPosition(), light->radius);
        gl.colorIntensity = glm::vec4(light->color, light->intensity);
        lightVec.push_back(gl);

        if (shadowIdx >= MAX_SHADOW_LIGHTS) continue;

        glClear(GL_DEPTH_BUFFER_BIT); // must clear per light

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
        sd.lightIndex = lightBufIdx; // absolute index into lights[] SSBO
        sd.farPlane = farPlane;
        shadowDataVec.push_back(sd);
        shadowIdx++;
    }

    m_lightCount = (int)lightVec.size();
    m_shadowCount = shadowIdx;

    // Upload light SSBO
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
    if (m_lightCount > 0)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUPointLight) * m_lightCount, lightVec.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Upload shadow SSBO
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
//  ShadingPass
// =====================================================
void RenderSystem::ShadingPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& cameraPos)
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    // Both MRT attachments must be active for the GBuffer write in ggx.frag
    GLenum drawBufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBufs);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);      // binding 0 — lights[]
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_shadowDataSSBO); // binding 1 — shadows[]

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh) continue;

        glm::mat4 model = transform->getModelMatrix();
        meshC->mesh->bindMaterial(model, view, projection);

        if (Material* mat = meshC->mesh->getMaterial()) {
            mat->setVec3("uCameraPos", cameraPos);
            mat->setInt("uShadowCubeArray", 5);
            mat->setInt("uShadowCount", m_shadowCount);
            mat->setInt("uLightCount", m_lightCount);
            mat->setInt("uShadowRes", m_shadowRes);
            mat->setMat4("uView", view); // needed by ggx.frag for view-space normal output
        }
        meshC->mesh->draw();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// =====================================================
//  TonemapPass
//  Tonemaps HDR color + composites bloom
//  and gamma-corrects to LDR FBO.
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

    // Bloom composite
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
//  1. Threshold pass  — extract bright pixels into m_bloomThreshTex
//  2. Downsample      — blit threshold to half-res ping buffer
//  3. Kawase N passes — ping-pong between ping/pong buffers
// =====================================================
void RenderSystem::BloomPass()
{
    const auto& rs = RenderSettings::instance();
    const int   bW = std::max(1, m_screenW / 2);
    const int   bH = std::max(1, m_screenH / 2);

    glDisable(GL_DEPTH_TEST);

    // ---- 1. Threshold ----
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

    // ---- 2. Downsample threshold -> ping (half res) ----
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

    // ---- 3. Kawase ping-pong at half res ----
    const int passes = rs.getBloomPasses();
    GLuint src = m_bloomPingTex;
    GLuint dstFBO = m_bloomPongFBO;
    GLuint srcFBO = m_bloomPingFBO; // unused but keeps naming clear

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

        // Swap ping/pong
        if (dstFBO == m_bloomPongFBO) {
            src = m_bloomPongTex;
            dstFBO = m_bloomPingFBO;
        }
        else {
            src = m_bloomPingTex;
            dstFBO = m_bloomPongFBO;
        }
    }
    // After the loop, 'src' holds the last written texture.
    // We always want the result in m_bloomPingTex so TonemapPass
    // can sample it without needing to know which buffer won.
    if (src != m_bloomPingTex) {
        // Copy pong -> ping via one more blit
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
//  Reads m_ldrTex (tonemap output) and writes the
//  anti-aliased result to the default framebuffer.
// =====================================================
void RenderSystem::FXAAPass()
{
    const auto& rs = RenderSettings::instance();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!rs.getFXAAEnabled()) {
        // No FXAA — just blit the LDR texture with the Kawase shader reused
        // as a simple pass-through.  Use the tonemap shader instead: easier
        // and cheaper — just sample the LDR tex and output it unchanged.
        // We repurpose the Kawase shader at iteration 0 as a copy blit.
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