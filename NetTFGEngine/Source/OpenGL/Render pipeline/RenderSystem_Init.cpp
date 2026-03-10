#include "RenderSystem.hpp"

// =====================================================
//  Shader compilation helpers (static)
// =====================================================
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

// =====================================================
//  InitDepthFBO
//  Attaches m_hdrDepthTex (created in InitHDRFBO) so
//  the depth pre-pass and shading pass share one surface.
//  Call AFTER InitHDRFBO.
// =====================================================
void RenderSystem::InitDepthFBO()
{
    glGenFramebuffers(1, &m_depthFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_2D, m_hdrDepthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "Depth FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// =====================================================
//  InitSSBOs
// =====================================================
void RenderSystem::InitSSBOs()
{
    int totalTiles = m_tilesX * m_tilesY;

    glGenBuffers(1, &m_lightSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        sizeof(GPUPointLight) * MAX_LIGHTS, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &m_lightIndexSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightIndexSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        sizeof(uint32_t) * totalTiles * MAX_LIGHTS_TILE, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &m_tileGridSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_tileGridSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        sizeof(GPUTileData) * totalTiles, nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// =====================================================
//  InitShadowCubeArray
// =====================================================
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

// =====================================================
//  InitHDRFBO
// =====================================================
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
    // GL_NONE: sampler2D in the compute shader reads raw [0,1] depth.
    // Default (GL_COMPARE_REF_TO_TEXTURE) would return 0 or 1 only.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    glGenFramebuffers(1, &m_hdrFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, m_hdrColorTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_2D, m_hdrDepthTex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "HDR FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// =====================================================
//  InitScreenQuad  � single large triangle covering NDC
// =====================================================
void RenderSystem::InitScreenQuad()
{
    static const float kVerts[] = {
        // position (NDC)   texcoord
        -1.0f, -1.0f,       0.0f, 0.0f,
         3.0f, -1.0f,       2.0f, 0.0f,
        -1.0f,  3.0f,       0.0f, 2.0f,
    };

    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &m_quadVBO);

    glBindVertexArray(m_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0); // vec2 position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(1); // vec2 texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
        4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// =====================================================
//  InitBloom
// =====================================================
void RenderSystem::InitBloom()
{
    int bW = std::max(1, m_screenW / 2);
    int bH = std::max(1, m_screenH / 2);

    auto makeBloomTex = [&](GLuint& tex, GLuint& fbo, int w, int h) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h,
            0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "Bloom FBO incomplete\n";
        };

    makeBloomTex(m_bloomThreshTex, m_bloomThreshFBO, m_screenW, m_screenH);
    makeBloomTex(m_bloomPingTex, m_bloomPingFBO, bW, bH);
    makeBloomTex(m_bloomPongTex, m_bloomPongFBO, bW, bH);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// =====================================================
//  InitFXAA
// =====================================================
void RenderSystem::InitFXAA()
{
    glGenTextures(1, &m_fxaaTex);
    glBindTexture(GL_TEXTURE_2D, m_fxaaTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
        m_screenW, m_screenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &m_fxaaFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fxaaFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, m_fxaaTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "FXAA FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}
// =====================================================
//  InitBloomBlackTex
//  1x1 black RGBA16F used as the bloom texture when
//  bloom is disabled so the tonemap shader never samples
//  texture name 0 (which is not a valid bound texture).
// =====================================================
void RenderSystem::InitBloomBlackTex()
{
    const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glGenTextures(1, &m_bloomBlackTex);
    glBindTexture(GL_TEXTURE_2D, m_bloomBlackTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 1, 1,
        0, GL_RGBA, GL_FLOAT, black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}