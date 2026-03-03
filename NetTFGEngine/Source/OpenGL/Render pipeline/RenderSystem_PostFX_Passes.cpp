#include "RenderSystem.hpp"

// =====================================================
//  SSAOPass
//  1. Render raw occlusion into m_ssaoFBO
//  2. Box-blur into m_ssaoBlurFBO
//  TonemapPass reads m_ssaoBlurTex.
// =====================================================
void RenderSystem::SSAOPass(const glm::mat4& projection)
{
    const auto& rs = RenderSettings::instance();
    glm::mat4 invProj = glm::inverse(projection);

    // Step 1: generate raw SSAO
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
    glViewport(0, 0, m_screenW, m_screenH);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_ssaoShader);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrDepthTex);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "uDepthTex"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTex);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "uNoiseTex"), 1);

    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "uInvProj"),
        1, GL_FALSE, glm::value_ptr(invProj));
    glUniform1i(glGetUniformLocation(m_ssaoShader, "uKernelSize"), rs.getSSAOSamples());
    glUniform1f(glGetUniformLocation(m_ssaoShader, "uRadius"), rs.getSSAORadius());
    glUniform1f(glGetUniformLocation(m_ssaoShader, "uBias"), rs.getSSAOBias());
    glUniform1f(glGetUniformLocation(m_ssaoShader, "uPower"), rs.getSSAOPower());
    glUniform2i(glGetUniformLocation(m_ssaoShader, "uScreenSize"), m_screenW, m_screenH);

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Step 2: blur SSAO
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_ssaoBlurShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssaoTex);
    glUniform1i(glGetUniformLocation(m_ssaoBlurShader, "uSSAOTex"), 0);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_DEPTH_TEST);
}

// =====================================================
//  BloomPass
//  1. Extract bright pixels (threshold) at full-res
//  2. Downsample to half-res ping buffer
//  3. Kawase ping-pong blur for N passes
// =====================================================
void RenderSystem::BloomPass()
{
    const auto& rs = RenderSettings::instance();
    int   passes = glm::clamp(rs.getBloomPasses(), 1, 8);
    float threshold = rs.getBloomThreshold();
    int   bW = std::max(1, m_screenW / 2);
    int   bH = std::max(1, m_screenH / 2);

    glDisable(GL_DEPTH_TEST);

    // Step 1: threshold extract
    glBindFramebuffer(GL_FRAMEBUFFER, m_bloomThreshFBO);
    glViewport(0, 0, m_screenW, m_screenH);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_bloomThreshShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
    glUniform1i(glGetUniformLocation(m_bloomThreshShader, "uHDRBuffer"), 0);
    glUniform1f(glGetUniformLocation(m_bloomThreshShader, "uThreshold"), threshold);

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Step 2: downsample to half-res ping
    glBindFramebuffer(GL_FRAMEBUFFER, m_bloomPingFBO);
    glViewport(0, 0, bW, bH);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_bloomKawaseShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bloomThreshTex);
    glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uBloomTex"), 0);
    glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uIteration"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Step 3: Kawase ping-pong
    GLuint src = m_bloomPingTex;
    GLuint dstFBO = m_bloomPongFBO;

    for (int i = 1; i < passes; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, dstFBO);
        glClear(GL_COLOR_BUFFER_BIT);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uBloomTex"), 0);
        glUniform1i(glGetUniformLocation(m_bloomKawaseShader, "uIteration"), i);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        if (dstFBO == m_bloomPongFBO) { dstFBO = m_bloomPingFBO; src = m_bloomPongTex; }
        else { dstFBO = m_bloomPongFBO; src = m_bloomPingTex; }
    }

    m_bloomResultTex = src; // last written texture

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
    glEnable(GL_DEPTH_TEST);
}

// =====================================================
//  SSRPass
// =====================================================
void RenderSystem::SSRPass(const glm::mat4& view, const glm::mat4& projection)
{
    const auto& rs = RenderSettings::instance();

    glBindFramebuffer(GL_FRAMEBUFFER, m_ssrFBO);
    glViewport(0, 0, m_screenW, m_screenH);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_ssrShader);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrDepthTex);
    glUniform1i(glGetUniformLocation(m_ssrShader, "uDepthTex"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
    glUniform1i(glGetUniformLocation(m_ssrShader, "uHDRColor"), 1);

    glm::mat4 invProj = glm::inverse(projection);
    glm::mat4 invView = glm::inverse(view);

    glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uInvProj"),
        1, GL_FALSE, glm::value_ptr(invProj));
    glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uView"),
        1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(glGetUniformLocation(m_ssrShader, "uInvView"),
        1, GL_FALSE, glm::value_ptr(invView));

    glUniform1i(glGetUniformLocation(m_ssrShader, "uMaxSteps"),
        rs.getSSRMaxSteps());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uMaxDistance"),
        rs.getSSRMaxDistance());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uStepSize"),
        rs.getSSRStepSize());
    glUniform1i(glGetUniformLocation(m_ssrShader, "uBinarySteps"),
        rs.getSSRBinarySteps());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uRoughnessCutoff"),
        rs.getSSRRoughnessCutoff());
    glUniform1f(glGetUniformLocation(m_ssrShader, "uFadeDistance"),
        rs.getSSRFadeDistance());

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_DEPTH_TEST);
}

// =====================================================
//  TonemapPass
//  Composites SSAO + SSR + Bloom onto HDR, then
//  tonemaps + gamma-corrects to LDR (-> fxaaFBO or FB 0).
// =====================================================
void RenderSystem::TonemapPass()
{
    const auto& rs = RenderSettings::instance();
    bool fxaaOn = rs.getFXAAEnabled();

    glBindFramebuffer(GL_FRAMEBUFFER, fxaaOn ? m_fxaaFBO : 0);
    glViewport(0, 0, m_screenW, m_screenH);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_tonemapShader);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uHDRBuffer"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, rs.getSSAOEnabled() ? m_ssaoBlurTex : 0);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uSSAOTex"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D,
        (rs.getBloomEnabled() && m_bloomResultTex) ? m_bloomResultTex : 0);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uBloomTex"), 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, rs.getSSREnabled() ? m_ssrTex : 0);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uSSRTex"), 3);

    glUniform1i(glGetUniformLocation(m_tonemapShader, "uSSAOEnabled"),
        rs.getSSAOEnabled() ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uBloomEnabled"),
        rs.getBloomEnabled() ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uBloomStrength"),
        rs.getBloomStrength());
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uSSREnabled"),
        rs.getSSREnabled() ? 1 : 0);

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

    glEnable(GL_DEPTH_TEST);
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

    glUseProgram(m_fxaaShader);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_fxaaTex);
    glUniform1i(glGetUniformLocation(m_fxaaShader, "uLDRBuffer"), 0);

    glUniform2f(glGetUniformLocation(m_fxaaShader, "uRcpFrame"),
        1.0f / (float)m_screenW, 1.0f / (float)m_screenH);
    glUniform1f(glGetUniformLocation(m_fxaaShader, "uEdgeThresholdMin"),
        rs.getFXAAEdgeThresholdMin());
    glUniform1f(glGetUniformLocation(m_fxaaShader, "uEdgeThreshold"),
        rs.getFXAAEdgeThreshold());
    glUniform1f(glGetUniformLocation(m_fxaaShader, "uSubpixel"),
        rs.getFXAASubpixel());

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
}

// =====================================================
//  BlitLDRToScreen  — no-op: TonemapPass already
//  wrote to FB 0 when FXAA is disabled.
// =====================================================
void RenderSystem::BlitLDRToScreen() { /* TonemapPass already wrote to FB 0 */ }