#include "RenderSystem.hpp"
#include <cmath>

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
//  InitLightSSBO
// =====================================================
void RenderSystem::InitLightSSBO()
{
    glGenBuffers(1, &m_lightSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        sizeof(GPUPointLight) * MAX_LIGHTS, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// =====================================================
//  InitShadowCubeArray  — point light cubemap array
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
//  InitDirLightUBO
//  Single GPUDirLight struct uploaded each frame.
//  Bound to uniform buffer binding 2.
// =====================================================
void RenderSystem::InitDirLightUBO()
{
    glGenBuffers(1, &m_dirLightUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_dirLightUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(GPUDirLight), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_dirLightUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

// =====================================================
//  InitDirShadowMap
//  A single 2-D DEPTH32F texture + FBO for the
//  directional light orthographic shadow map.
//  Resolution reuses getShadowResolution() (same as
//  point lights — change the setting before calling
//  ReInitShadows() to adjust both at once).
// =====================================================
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

// =====================================================
//  InitGBufferFBO
// =====================================================
void RenderSystem::InitGBufferFBO()
{
    auto makeAttachment = [&](GLuint& tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
            m_screenW, m_screenH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        };

    makeAttachment(m_gbufferNormalTex);
    makeAttachment(m_gbufferRoughnessTex);
    makeAttachment(m_gbufferMetalnessTex);

    glGenRenderbuffers(1, &m_gbufferDepthRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_gbufferDepthRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, m_screenW, m_screenH);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &m_gbufferFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, m_gbufferNormalTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
        GL_TEXTURE_2D, m_gbufferRoughnessTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
        GL_TEXTURE_2D, m_gbufferMetalnessTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_RENDERBUFFER, m_gbufferDepthRBO);

    const GLenum drawBufs[3] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2
    };
    glDrawBuffers(3, drawBufs);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "GBuffer FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// =====================================================
//  InitMSAAFBO
// =====================================================
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

// =====================================================
//  InitScreenQuad
// =====================================================
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

// =====================================================
//  InitBloom
// =====================================================
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

// =====================================================
//  InitLDRFBO
// =====================================================
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