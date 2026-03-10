#include "RenderSystem.hpp"

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
    glBindTexture(GL_TEXTURE_2D,
        (rs.getBloomEnabled() && m_bloomResultTex) ? m_bloomResultTex : m_bloomBlackTex);
    glUniform1i(glGetUniformLocation(m_tonemapShader, "uBloomTex"), 1);

    glUniform1i(glGetUniformLocation(m_tonemapShader, "uBloomEnabled"),
        rs.getBloomEnabled() ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_tonemapShader, "uBloomStrength"),
        rs.getBloomStrength());

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
//  BlitLDRToScreen  � no-op: TonemapPass already
//  wrote to FB 0 when FXAA is disabled.
// =====================================================
void RenderSystem::BlitLDRToScreen() { /* TonemapPass already wrote to FB 0 */ }