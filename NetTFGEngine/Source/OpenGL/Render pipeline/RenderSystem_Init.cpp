#include "RenderSystem.hpp"
#include <cmath>

// Shader compilation helpers (static)
GLuint RenderSystem::CompileStage(GLenum type, const char* src)
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

GLuint RenderSystem::LinkProgram(std::initializer_list<GLuint> stages)
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

void RenderSystem::InitLightSSBO()
{
    glGenBuffers(1, &m_lightSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        sizeof(GPUPointLight) * MAX_LIGHTS, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void RenderSystem::InitShadowCubeArray()
{
    m_shadowRes = RenderSettings::instance().getShadowResolution();

    glGenTextures(1, &m_shadowCubeArray);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);
    glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_DEPTH_COMPONENT32F,
        m_shadowRes, m_shadowRes, MAX_SHADOW_LIGHTS * 6,
        0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &m_shadowFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadowCubeArray, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glGenBuffers(1, &m_shadowDataSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowDataSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        sizeof(GPUShadowData) * MAX_SHADOW_LIGHTS, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// Single GPUDirLight struct uploaded each frame, bound to uniform buffer binding 2.
void RenderSystem::InitDirLightUBO()
{
    glGenBuffers(1, &m_dirLightUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_dirLightUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(GPUDirLight), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_dirLightUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

// A single 2-D DEPTH32F texture + FBO for the directional light's orthographic shadow map; resolution is shared with point lights via getShadowResolution().
void RenderSystem::InitDirShadowMap()
{
    int res = RenderSettings::instance().getDirShadowResolution();

    glGenTextures(1, &m_dirShadowTex);
    glBindTexture(GL_TEXTURE_2D, m_dirShadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
        res, res, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    // Use hardware PCF comparison sampler so the shader can use shadow2D().
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    // Fragments outside the frustum are treated as fully lit.
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LESS);

    glGenFramebuffers(1, &m_dirShadowFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_dirShadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_2D, m_dirShadowTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "Dir shadow FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void RenderSystem::InitGBufferFBO()
{
    // 20 bytes/pixel: only the normal and the velocity need half-float precision; metalness, the env flag and albedo
    // fit in 8 bits.
    auto makeAttachment = [&](GLuint& tex, GLenum internalFormat, GLenum type) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat,
            m_screenW, m_screenH, 0, GL_RGBA, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        };

    makeAttachment(m_gbufferNormalTex, GL_RGBA16F, GL_FLOAT);
    makeAttachment(m_gbufferMaterialTex, GL_RGBA8, GL_UNSIGNED_BYTE);
    makeAttachment(m_gbufferAlbedoTex, GL_RGBA8, GL_UNSIGNED_BYTE);
    makeAttachment(m_gbufferVelocityTex, GL_RG16F, GL_FLOAT);

    glGenTextures(1, &m_gbufferDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
        m_screenW, m_screenH, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &m_gbufferFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, m_gbufferNormalTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
        GL_TEXTURE_2D, m_gbufferMaterialTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
        GL_TEXTURE_2D, m_gbufferAlbedoTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3,
        GL_TEXTURE_2D, m_gbufferVelocityTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_2D, m_gbufferDepthTex, 0);

    const GLenum drawBufs[4] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3
    };
    glDrawBuffers(4, drawBufs);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "GBuffer FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void RenderSystem::InitMSAAFBO()
{
    if (m_msaaSamples <= 1)
        return;

    glGenTextures(1, &m_msaaColorTex);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_msaaColorTex);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_msaaSamples,
        GL_RGBA16F, m_screenW, m_screenH, GL_TRUE);

    glGenTextures(1, &m_msaaDepthTex);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_msaaDepthTex);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_msaaSamples,
        GL_DEPTH_COMPONENT32F, m_screenW, m_screenH, GL_TRUE);

    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

    glGenFramebuffers(1, &m_msaaFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D_MULTISAMPLE, m_msaaColorTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_2D_MULTISAMPLE, m_msaaDepthTex, 0);

    GLenum drawBufs[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBufs);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "MSAA FBO incomplete (samples="
        << m_msaaSamples << ")\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderSystem::InitHDRFBO()
{
    glGenTextures(1, &m_hdrColorTex);
    glBindTexture(GL_TEXTURE_2D, m_hdrColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
        m_screenW, m_screenH, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_hdrDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_hdrDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
        m_screenW, m_screenH, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &m_hdrFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, m_hdrColorTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_2D, m_hdrDepthTex, 0);

    GLenum drawBufs[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBufs);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "HDR FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void RenderSystem::InitScreenQuad()
{
    static const float kVerts[] = {
        -1.0f, -1.0f,  0.0f, 0.0f,
         3.0f, -1.0f,  2.0f, 0.0f,
        -1.0f,  3.0f,  0.0f, 2.0f,
    };

    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &m_quadVBO);

    glBindVertexArray(m_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RenderSystem::InitBloom()
{
    auto makeTex = [&](GLuint& tex, GLuint& fbo, int w, int h) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "Bloom FBO incomplete\n";
        };

    int bW = std::max(1, m_screenW / 2);
    int bH = std::max(1, m_screenH / 2);
    makeTex(m_bloomThreshTex, m_bloomThreshFBO, m_screenW, m_screenH);
    makeTex(m_bloomPingTex, m_bloomPingFBO, bW, bH);
    makeTex(m_bloomPongTex, m_bloomPongFBO, bW, bH);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Screen-space effect targets. SSAO and SSR at the render resolution divided by their scale setting (sizes rounded up:
// reduced pixel p covers full pixels s*p .. s*p+s-1).
// The SSAO kernel is built once: hemisphere directions (z up in tangent space) with a random length; the SSAO shader
// scales them so samples cluster near the pixel whatever sample count is in use.
void RenderSystem::InitScreenSpace()
{
    const auto& rs = RenderSettings::instance();
    m_ssaoScale = rs.getSSAOResolutionScale();
    m_ssrScale  = rs.getSSRResolutionScale();
    m_ssaoW = std::max(1, (m_screenW + m_ssaoScale - 1) / m_ssaoScale);
    m_ssaoH = std::max(1, (m_screenH + m_ssaoScale - 1) / m_ssaoScale);
    m_ssrW  = std::max(1, (m_screenW + m_ssrScale - 1) / m_ssrScale);
    m_ssrH  = std::max(1, (m_screenH + m_ssrScale - 1) / m_ssrScale);

    auto makeTarget = [&](GLuint& tex, GLuint& fbo, int w, int h, GLenum internalFormat, GLenum format, GLenum type,
                          GLenum filter, const char* name) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << name << " FBO incomplete\n";
        };

    makeTarget(m_linearDepthTex, m_linearDepthFBO, m_ssaoW, m_ssaoH, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST, "Linear depth");
    if (m_ssrScale != m_ssaoScale)
        makeTarget(m_ssrLinearDepthTex, m_ssrLinearDepthFBO, m_ssrW, m_ssrH, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST, "SSR linear depth");
    makeTarget(m_ssaoTex, m_ssaoFBO, m_ssaoW, m_ssaoH, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST, "SSAO");
    // Bilinear: the shading pass upscales it with texture().
    makeTarget(m_ssaoBlurTex, m_ssaoBlurFBO, m_ssaoW, m_ssaoH, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_LINEAR, "SSAO blur");
    makeTarget(m_ssrTraceTex, m_ssrTraceFBO, m_ssrW, m_ssrH, GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_NEAREST, "SSR trace");

    // Motion blur. The tile size is also the max blur radius, so it follows the resolution (~same look at 1080p/4K).
    m_mbTile = std::clamp(m_screenH / 64, 8, 40);
    m_mbTilesW = std::max(1, (m_screenW + m_mbTile - 1) / m_mbTile);
    m_mbTilesH = std::max(1, (m_screenH + m_mbTile - 1) / m_mbTile);
    auto makeSized = [&](GLuint& tex, GLuint& fbo, int w, int h, GLenum internalFormat, GLenum format,
                         GLenum filter, const char* name) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << name << " FBO incomplete\n";
        };
    makeSized(m_mbVelocityTex, m_mbVelocityFBO, m_screenW, m_screenH, GL_RG16F, GL_RG, GL_NEAREST, "Motion blur velocity");
    makeSized(m_mbTileMaxTex, m_mbTileMaxFBO, m_mbTilesW, m_mbTilesH, GL_RG16F, GL_RG, GL_NEAREST, "Motion blur tile max");
    makeSized(m_mbNeighborMaxTex, m_mbNeighborMaxFBO, m_mbTilesW, m_mbTilesH, GL_RG16F, GL_RG, GL_NEAREST, "Motion blur neighbour max");
    // Bilinear: the gather samples it at fractional positions along the blur.
    makeSized(m_mbSourceTex, m_mbSourceFBO, m_screenW, m_screenH, GL_RGBA16F, GL_RGBA, GL_LINEAR, "Motion blur source");

    // Start fully unoccluded so nothing reads garbage before the first SSAOPass.
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    // SSR scene copy: immutable storage, only the mips the SSR shader reads (lod <= 5 at full res, 4 at half, 3 at
    // quarter: the blurriest mip covers the same screen area at any scale); glGenerateMipmap then doesn't waste time on
    // the tiny ones.
    const int fullChain = 1 + (int)std::floor(std::log2((float)std::max(m_ssrW, m_ssrH)));
    const int scaleLevels = (m_ssrScale >= 4) ? 2 : (m_ssrScale >= 2 ? 1 : 0);
    m_ssrSceneMips = std::max(1, std::min(fullChain, 6 - scaleLevels));

    glGenTextures(1, &m_ssrSceneTex);
    glBindTexture(GL_TEXTURE_2D, m_ssrSceneTex);
    glTexStorage2D(GL_TEXTURE_2D, m_ssrSceneMips, GL_RGBA16F, m_ssrW, m_ssrH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &m_ssrSceneFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssrSceneFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssrSceneTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "SSR scene FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (m_ssaoKernel.empty()) {
        // Fixed seed: the pattern is part of the look, it shouldn't change between runs.
        uint32_t state = 0x9E3779B9u;
        auto rnd = [&state]() {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            return float(state & 0xFFFFFFu) / float(0x1000000);
            };

        m_ssaoKernel.reserve(64);
        while (m_ssaoKernel.size() < 64) {
            glm::vec3 d(rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, rnd());
            const float len = glm::length(d);
            if (len < 0.1f || len > 1.0f) continue; // rejection sampling keeps the hemisphere uniform
            m_ssaoKernel.push_back(d / len * rnd());
        }
    }
}

void RenderSystem::DeleteScreenSpace()
{
    // glDelete* ignores 0 names, so the SSR linear depth (only built when the scales differ) can go in unconditionally.
    GLuint fbos[] = { m_linearDepthFBO, m_ssrLinearDepthFBO, m_ssaoFBO, m_ssaoBlurFBO, m_ssrTraceFBO, m_ssrSceneFBO,
                      m_mbVelocityFBO, m_mbTileMaxFBO, m_mbNeighborMaxFBO, m_mbSourceFBO };
    GLuint texs[] = { m_linearDepthTex, m_ssrLinearDepthTex, m_ssaoTex, m_ssaoBlurTex, m_ssrTraceTex, m_ssrSceneTex,
                      m_mbVelocityTex, m_mbTileMaxTex, m_mbNeighborMaxTex, m_mbSourceTex };
    glDeleteFramebuffers(10, fbos);
    glDeleteTextures(10, texs);
    m_linearDepthFBO = m_ssrLinearDepthFBO = m_ssaoFBO = m_ssaoBlurFBO = m_ssrTraceFBO = m_ssrSceneFBO = 0;
    m_linearDepthTex = m_ssrLinearDepthTex = m_ssaoTex = m_ssaoBlurTex = m_ssrTraceTex = m_ssrSceneTex = 0;
    m_mbVelocityFBO = m_mbTileMaxFBO = m_mbNeighborMaxFBO = m_mbSourceFBO = 0;
    m_mbVelocityTex = m_mbTileMaxTex = m_mbNeighborMaxTex = m_mbSourceTex = 0;
}

void RenderSystem::InitLDRFBO()
{
    glGenTextures(1, &m_ldrTex);
    glBindTexture(GL_TEXTURE_2D, m_ldrTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
        m_screenW, m_screenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &m_ldrFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ldrFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ldrTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "LDR FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// RGBA8 like the LDR target: scaling works on the tonemapped, gamma-encoded image. The nearest blit sets its own
// filter, EASU gathers and RCAS fetches texels directly, so the filter mode doesn't matter; linear/clamp just matches
// the other screen targets.
static void CreateScaleTarget(GLuint& fbo, GLuint& tex, int w, int h, const char* name)
{
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << name << " FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void RenderSystem::EnsureScaleTargets(bool withEasu)
{
    if (m_preScaleTex == 0 || m_preScaleW != m_screenW || m_preScaleH != m_screenH) {
        glDeleteFramebuffers(1, &m_preScaleFBO);
        glDeleteTextures(1, &m_preScaleTex);
        CreateScaleTarget(m_preScaleFBO, m_preScaleTex, m_screenW, m_screenH, "Pre-scale");
        m_preScaleW = m_screenW;
        m_preScaleH = m_screenH;
    }

    if (!withEasu) {
        // Switched away from FSR: its output-size target isn't needed any more.
        glDeleteFramebuffers(1, &m_fsrEasuFBO); m_fsrEasuFBO = 0;
        glDeleteTextures(1, &m_fsrEasuTex);     m_fsrEasuTex = 0;
        m_fsrEasuW = m_fsrEasuH = 0;
        return;
    }
    if (m_fsrEasuTex == 0 || m_fsrEasuW != m_outputW || m_fsrEasuH != m_outputH) {
        glDeleteFramebuffers(1, &m_fsrEasuFBO);
        glDeleteTextures(1, &m_fsrEasuTex);
        CreateScaleTarget(m_fsrEasuFBO, m_fsrEasuTex, m_outputW, m_outputH, "FSR EASU");
        m_fsrEasuW = m_outputW;
        m_fsrEasuH = m_outputH;
    }
}

void RenderSystem::DeleteScaleTargets()
{
    glDeleteFramebuffers(1, &m_preScaleFBO); m_preScaleFBO = 0;
    glDeleteTextures(1, &m_preScaleTex);     m_preScaleTex = 0;
    glDeleteFramebuffers(1, &m_fsrEasuFBO);  m_fsrEasuFBO = 0;
    glDeleteTextures(1, &m_fsrEasuTex);      m_fsrEasuTex = 0;
    m_preScaleW = m_preScaleH = m_fsrEasuW = m_fsrEasuH = 0;
}