#include "RenderSystem.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stb_image.h>

// Environment lighting (image-based lighting). What every lit material samples through EnvLighting.hpp:
//
//   1. BRDF LUT (once, Init): the GGX specular BRDF integrated over the hemisphere for each (NdotV, roughness), as a
//      scale and bias applied to F0 - the second factor of Karis' split sum.
//   2. Sky (layer 0): EnvironmentSkyComponent's cube map (six image assets, or one equirectangular panorama converted
//      on the GPU, chosen by the game), loaded into a float cube map once, resampled into the capture cube (oriented
//      and scaled as the component says), then prefiltered.
//   3. Probes (layers 1..N): the static scene rendered from each ReflectionProbeComponent into the capture cube (with
//      the sky behind it and lit by the sky's environment), then prefiltered. A few per frame until all are done.
//
// "Prefiltered" = the first factor of the split sum: for each mip m of the array, the capture convolved with the GGX
// lobe of perceptual roughness m / (mips - 1) (importance sampled, reading the capture's own mips by the sample's
// solid angle so a few hundred samples don't sparkle on small bright spots: filtered importance sampling, Krivanek &
// Colbert 2008). The diffuse part is 9 spherical harmonics coefficients per layer, projected on the CPU from a 32x32
// mip of the capture.
//
// Nothing here runs per frame once the maps are built: EnvironmentPass only compares hashes and uploads the probe
// SSBO (a few KB).

namespace {

    constexpr int kBrdfLutSize = 256;
    constexpr int kBrdfLutSamples = 1024;
    constexpr int kPrefilterSamples = 256;   // enough with filtered importance sampling (no sparkles)
    constexpr int kMinEnvMipSize = 8;      // last prefiltered mip: 8x8 faces is already a fully rough blur
    constexpr int kSHSampleSize = 32;      // capture mip read back for the SH projection
    // Captures per frame while some are pending: each is 6 scene renders (+ their shadow cascades), a prefilter and
    // a small readback. Two keeps a mid-match recapture (a tile dropping out) to a few short frames, not one long one.
    constexpr int kProbesPerFrame = 2;

    // GL cube face order (+X, -X, +Y, -Y, +Z, -Z): view direction and up vector of each face's capture camera. The
    // same convention as ShadowPass, and the one FaceDir() (shaders and CPU) inverts.
    const glm::vec3 kFaceDir[6] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    const glm::vec3 kFaceUp[6] = {
        { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 },
    };

    // Direction through texel (u, v) in [-1, 1] of a cube face, v growing with the texel row (= gl_FragCoord.y when
    // rendering into the face, = row order of glGetTexImage).
    glm::vec3 FaceDir(int face, float u, float v)
    {
        switch (face) {
        case 0:  return {  1.0f,   -v,   -u };
        case 1:  return { -1.0f,   -v,    u };
        case 2:  return {     u, 1.0f,    v };
        case 3:  return {     u, -1.0f,  -v };
        case 4:  return {     u,   -v, 1.0f };
        default: return {    -u,   -v, -1.0f };
        }
    }

    // FNV-1a over the values a capture depends on: a changed hash = re-render.
    struct Hasher {
        uint64_t h = 1469598103934665603ull;
        void bytes(const void* p, size_t n)
        {
            const auto* b = static_cast<const unsigned char*>(p);
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        }
        void f(float v) { bytes(&v, sizeof v); }
        void i(int64_t v) { bytes(&v, sizeof v); }
        void v3(const glm::vec3& v) { f(v.x); f(v.y); f(v.z); }
        void v4(const glm::vec4& v) { f(v.x); f(v.y); f(v.z); f(v.w); }
    };

    // Fullscreen triangle (m_quadVAO); every environment shader works from gl_FragCoord.
    const char* kEnvVert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    // Shared by the sky and prefilter shaders: the GLSL twin of FaceDir() above.
    const char* kFaceDirGLSL = R"GLSL(
        vec3 FaceDir(int face, vec2 uv)
        {
            if (face == 0) return vec3( 1.0, -uv.y, -uv.x);
            if (face == 1) return vec3(-1.0, -uv.y,  uv.x);
            if (face == 2) return vec3( uv.x,  1.0,  uv.y);
            if (face == 3) return vec3( uv.x, -1.0, -uv.y);
            if (face == 4) return vec3( uv.x, -uv.y,  1.0);
            return vec3(-uv.x, -uv.y, -1.0);
        }
    )GLSL";

    // GGX importance sampling (Hammersley points), shared by the prefilter and the LUT.
    const char* kGGXSamplingGLSL = R"GLSL(
        const float PI = 3.14159265358979;

        vec2 Hammersley(uint i, uint n)
        {
            uint b = i;
            b = (b << 16u) | (b >> 16u);
            b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
            b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
            b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
            b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
            return vec2(float(i) / float(n), float(b) * 2.3283064365386963e-10);
        }

        // Half vector around N distributed as D_GGX(alpha) * NdotH.
        vec3 ImportanceSampleGGX(vec2 xi, vec3 N, float alpha)
        {
            float a2       = alpha * alpha;
            float phi      = 2.0 * PI * xi.x;
            float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
            float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
            vec3  H        = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

            vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
            vec3 T  = normalize(cross(up, N));
            vec3 B  = cross(N, T);
            return normalize(T * H.x + B * H.y + N * H.z);
        }

        float D_GGX(float NdotH, float alpha)
        {
            float a2 = alpha * alpha;
            float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
            return a2 / (PI * d * d);
        }
    )GLSL";

    // Sky cube map orientation: its +Y turned onto upAxis by the shortest rotation, then spun around upAxis. Returns
    // world -> cube map (the inverse of that rotation), what the sky shader needs to look a world direction up.
    glm::mat3 WorldToSkyRotation(const EnvironmentSkyComponent& sky)
    {
        const glm::vec3 y(0.0f, 1.0f, 0.0f);
        const glm::vec3 up = glm::length(sky.upAxis) > 1e-6f ? glm::normalize(sky.upAxis) : y;

        glm::mat4 skyToWorld(1.0f);
        const glm::vec3 axis = glm::cross(y, up);
        const float d = glm::clamp(glm::dot(y, up), -1.0f, 1.0f);
        if (glm::length(axis) > 1e-6f)
            skyToWorld = glm::rotate(glm::mat4(1.0f), std::acos(d), glm::normalize(axis));
        else if (d < 0.0f)   // upside down: any half turn about a horizontal axis
            skyToWorld = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        skyToWorld = glm::rotate(glm::mat4(1.0f), glm::radians(sky.rotationDegrees), up) * skyToWorld;

        return glm::transpose(glm::mat3(skyToWorld));   // rotation: inverse = transpose
    }

    void HashSkyFaces(Hasher& h, const EnvironmentSkyComponent& sky)
    {
        for (const std::string& f : sky.faces) { h.bytes(f.data(), f.size()); h.i((int64_t)f.size()); }
        h.bytes(sky.equirect.data(), sky.equirect.size());
        h.i((int64_t)sky.equirect.size());
    }

    void HashSky(Hasher& h, const EnvironmentSkyComponent& sky)
    {
        HashSkyFaces(h, sky);
        h.f(sky.intensity); h.v3(sky.upAxis); h.f(sky.rotationDegrees);
    }

    float SrgbToLinear(uint8_t v)
    {
        const float c = v / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

} // namespace

void RenderSystem::CompileEnvironmentShaders()
{
    // Sky cube map resampled into face uFace of the bound cube target: each texel looks its world direction up in the
    // game's cube map (rotated by uWorldToSky), scaled by the component's intensity. uLod picks the source mip whose
    // texels match the target's, so a big source shrinks without aliasing (stars don't flicker in and out).
    const std::string skyFrag = std::string(R"GLSL(
        #version 430 core
        out vec4 FragColor;

        uniform samplerCube uSky;
        uniform int   uHasSky;        // 0: no sky cube map (none set, or it failed to load): black
        uniform int   uFace;
        uniform int   uSize;
        uniform mat3  uWorldToSky;
        uniform float uIntensity;
        uniform float uLod;
    )GLSL") + kFaceDirGLSL + R"GLSL(
        void main()
        {
            if (uHasSky == 0) { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
            vec2 uv = gl_FragCoord.xy / float(uSize) * 2.0 - 1.0;
            vec3 d  = normalize(FaceDir(uFace, uv));
            vec3 c  = textureLod(uSky, uWorldToSky * d, uLod).rgb * uIntensity;
            FragColor = vec4((any(isnan(c)) || any(isinf(c))) ? vec3(0.0) : max(c, vec3(0.0)), 1.0);
        }
    )GLSL";

    // One face of one mip of the prefiltered array: the capture cube convolved with the GGX lobe of uRoughness around
    // the texel's direction (N = V = R, the split-sum assumption). Mip 0 (roughness 0) is a plain copy.
    const std::string prefilterFrag = std::string(R"GLSL(
        #version 430 core
        out vec4 FragColor;

        uniform samplerCube uSource;      // capture cube, full mip chain
        uniform int   uFace;
        uniform int   uTargetSize;        // face size of the mip being written
        uniform float uSourceSize;        // face size of the capture's mip 0
        uniform float uRoughness;         // perceptual
        uniform int   uSampleCount;
    )GLSL") + kFaceDirGLSL + kGGXSamplingGLSL + R"GLSL(
        vec3 Fetch(vec3 dir, float lod)
        {
            vec3 c = textureLod(uSource, dir, lod).rgb;
            return (any(isnan(c)) || any(isinf(c))) ? vec3(0.0) : max(c, vec3(0.0));
        }

        void main()
        {
            vec2 uv = gl_FragCoord.xy / float(uTargetSize) * 2.0 - 1.0;
            vec3 N  = normalize(FaceDir(uFace, uv));

            if (uRoughness <= 0.0) { FragColor = vec4(Fetch(N, 0.0), 1.0); return; }

            float alpha   = uRoughness * uRoughness;
            float saTexel = 4.0 * PI / (6.0 * uSourceSize * uSourceSize);
            vec3  sum     = vec3(0.0);
            float wsum    = 0.0;
            uint  n       = uint(uSampleCount);
            for (uint i = 0u; i < n; ++i)
            {
                vec3  H     = ImportanceSampleGGX(Hammersley(i, n), N, alpha);
                float NdotH = clamp(dot(N, H), 0.0, 1.0);
                vec3  L     = normalize(2.0 * NdotH * H - N);
                float NdotL = dot(N, L);
                if (NdotL <= 0.0) continue;

                // pdf of L = D * NdotH / (4 * VdotH), and V = N here. The sample stands for a solid angle of
                // 1 / (n * pdf); reading the source mip whose texels cover that much keeps the estimate smooth.
                float pdf      = D_GGX(NdotH, alpha) * 0.25;
                float saSample = 1.0 / (float(n) * pdf + 1e-6);
                float lod      = max(0.5 * log2(saSample / saTexel) + 1.0, 0.0);

                sum  += Fetch(L, lod) * NdotL;
                wsum += NdotL;
            }
            FragColor = vec4(sum / max(wsum, 1e-6), 1.0);
        }
    )GLSL";

    // Split-sum BRDF LUT: x = NdotV, y = perceptual roughness (texel centres), RG = scale and bias of F0. Same height-
    // correlated Smith visibility as the direct lighting (DefaultShader.hpp), so both terms agree.
    const std::string brdfFrag = std::string(R"GLSL(
        #version 430 core
        out vec2 FragColor;

        uniform int uSize;
        uniform int uSampleCount;
    )GLSL") + kGGXSamplingGLSL + R"GLSL(
        float V_SmithGGXCorrelated(float NdotV, float NdotL, float alpha)
        {
            float a2   = alpha * alpha;
            float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
            float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
            return 0.5 / max(GGXV + GGXL, 1e-5);
        }

        void main()
        {
            float NdotV = gl_FragCoord.x / float(uSize);
            float rough = gl_FragCoord.y / float(uSize);
            float alpha = rough * rough;

            vec3 N = vec3(0.0, 0.0, 1.0);
            vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);

            float A = 0.0, B = 0.0;
            uint  n = uint(uSampleCount);
            for (uint i = 0u; i < n; ++i)
            {
                vec3  H     = ImportanceSampleGGX(Hammersley(i, n), N, alpha);
                float VdotH = clamp(dot(V, H), 0.0, 1.0);
                vec3  L     = 2.0 * VdotH * H - V;
                float NdotL = clamp(L.z, 0.0, 1.0);
                float NdotH = clamp(H.z, 0.0, 1.0);
                if (NdotL <= 0.0) continue;

                // f * NdotL / pdf with pdf = D * NdotH / (4 * VdotH): D cancels, leaving the visibility term.
                float Gv = V_SmithGGXCorrelated(NdotV, NdotL, alpha) * NdotL * 4.0 * VdotH / max(NdotH, 1e-5);
                float Fc = pow(1.0 - VdotH, 5.0);
                A += (1.0 - Fc) * Gv;
                B += Fc * Gv;
            }
            FragColor = vec2(A, B) / float(n);
        }
    )GLSL";

    m_envSkyShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kEnvVert),
        CompileStage(GL_FRAGMENT_SHADER, skyFrag.c_str())
        });
    m_envPrefilterShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kEnvVert),
        CompileStage(GL_FRAGMENT_SHADER, prefilterFrag.c_str())
        });
    m_envBrdfShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kEnvVert),
        CompileStage(GL_FRAGMENT_SHADER, brdfFrag.c_str())
        });

    // Equirectangular panorama -> one cube face (LoadSkyEquirect). Longitude 0 (u = 0.5, the image centre) looks
    // toward -Z with +X to its right, latitude from +Y (top row, t = 0: the image is uploaded unflipped) to -Y.
    // Explicit lod: implicit derivatives would jump across the u = 0/1 seam and read a 1-texel mip there (a line).
    const std::string equirectFrag = std::string(R"GLSL(
        #version 430 core
        out vec4 FragColor;

        uniform sampler2D uPanorama;
        uniform int   uFace;
        uniform int   uSize;
        uniform float uLod;
    )GLSL") + kFaceDirGLSL + R"GLSL(
        const float PI = 3.14159265358979;

        void main()
        {
            vec2 uv = gl_FragCoord.xy / float(uSize) * 2.0 - 1.0;
            vec3 d  = normalize(FaceDir(uFace, uv));
            vec2 st = vec2(0.5 + atan(d.x, -d.z) / (2.0 * PI),
                           acos(clamp(d.y, -1.0, 1.0)) / PI);
            vec3 c  = textureLod(uPanorama, st, uLod).rgb;
            FragColor = vec4((any(isnan(c)) || any(isinf(c))) ? vec3(0.0) : max(c, vec3(0.0)), 1.0);
        }
    )GLSL";

    m_envEquirectShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   kEnvVert),
        CompileStage(GL_FRAGMENT_SHADER, equirectFrag.c_str())
        });
}

void RenderSystem::InitEnvironment()
{
    m_envProbeData.clear();
    m_envProbeReady.clear();
    m_envProbeCount = 0;
    m_envSkyReady = false;
    m_envSkyHash = 0;
    m_envProbeSetHash = 0;

    glGenFramebuffers(1, &m_envFBO);

    // Probe SSBO at its maximum size once: entries are rewritten in place as probes come and go.
    std::vector<GPUEnvProbe> zero(1 + EnvLighting::kMaxProbes, GPUEnvProbe{});
    glGenBuffers(1, &m_envProbeSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_envProbeSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, zero.size() * sizeof(GPUEnvProbe), zero.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // BRDF LUT: depends on nothing but the BRDF, rendered once.
    glGenTextures(1, &m_envBrdfLUT);
    glBindTexture(GL_TEXTURE_2D, m_envBrdfLUT);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG16F, kBrdfLutSize, kBrdfLutSize);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, m_envFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_envBrdfLUT, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Debug::Error("RenderSystem") << "BRDF LUT FBO incomplete\n";
    glViewport(0, 0, kBrdfLutSize, kBrdfLutSize);
    glUseProgram(m_envBrdfShader);
    glUniform1i(glGetUniformLocation(m_envBrdfShader, "uSize"), kBrdfLutSize);
    glUniform1i(glGetUniformLocation(m_envBrdfShader, "uSampleCount"), kBrdfLutSamples);
    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glEnable(GL_DEPTH_TEST);
    if (blendWasOn) glEnable(GL_BLEND);
    glViewport(0, 0, m_screenW, m_screenH);
}

void RenderSystem::AllocateEnvironmentMaps(int res, int layers)
{
    DeleteEnvironmentMaps();

    m_envRes = res;
    m_envLayers = layers;
    m_envMips = 1;
    while ((res >> m_envMips) >= kMinEnvMipSize) ++m_envMips;

    glGenTextures(1, &m_envPrefiltered);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_envPrefiltered);
    glTexStorage3D(GL_TEXTURE_CUBE_MAP_ARRAY, m_envMips, GL_RGBA16F, res, res, layers * 6);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAX_LEVEL, m_envMips - 1);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);

    // Full chain on the capture: the prefilter reads coarser mips for wide samples, the SH projection a 32x32 one.
    const int captureLevels = 1 + (int)std::floor(std::log2((float)res));
    glGenTextures(1, &m_envCaptureCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCaptureCube);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, captureLevels, GL_RGBA16F, res, res);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    glGenRenderbuffers(1, &m_envCaptureDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_envCaptureDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, res, res);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    m_envProbeData.assign(layers, GPUEnvProbe{});
    m_envProbeReady.assign(layers, false);
    m_envSkyReady = false;

    Debug::Info("RenderSystem") << "Environment maps: " << res << "x" << res << ", " << m_envMips << " mips, "
        << layers << " layer(s) (sky + " << (layers - 1) << " probe(s))\n";
}

void RenderSystem::DeleteSkyCubemap()
{
    if (m_envSkySource) glDeleteTextures(1, &m_envSkySource);
    m_envSkySource = 0;
    m_envSkySourceSize = 0;
    m_envSkyFacesHash = 0;   // re-loaded from the assets next time
}

void RenderSystem::DeleteEnvironmentMaps()
{
    if (m_envPrefiltered)  glDeleteTextures(1, &m_envPrefiltered);
    if (m_envCaptureCube)  glDeleteTextures(1, &m_envCaptureCube);
    if (m_envCaptureDepth) glDeleteRenderbuffers(1, &m_envCaptureDepth);
    m_envPrefiltered = m_envCaptureCube = m_envCaptureDepth = 0;
    m_envRes = m_envMips = m_envLayers = 0;
    m_envSkyReady = false;
    m_envProbeCount = 0;
    m_envProbeData.clear();
    m_envProbeReady.clear();
}

bool RenderSystem::EnvironmentActive() const
{
    return RenderSettings::instance().getEnvLightingEnabled() && m_envSkyReady && m_envPrefiltered && m_envBrdfLUT;
}

void RenderSystem::EnvironmentPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery)
{
    const auto& rs = RenderSettings::instance();

    // Off: free the maps (~110 MB at 256 with the arena's 25 probes) and the sky cube map. Rebuilt from scratch when
    // switched back on.
    if (!rs.getEnvLightingEnabled()) {
        if (m_envPrefiltered) DeleteEnvironmentMaps();
        DeleteSkyCubemap();
        return;
    }
    if (!m_envSkyShader || !m_envPrefilterShader || !m_envBrdfLUT || !m_envFBO) return;

    EnvironmentSkyComponent sky{};
    {
        auto skyQuery = em.CreateQuery<EnvironmentSkyComponent>();
        for (auto [entity, s] : skyQuery) { sky = *s; break; }
    }

    struct ProbeRef { Entity entity; ReflectionProbeComponent* probe; glm::vec3 pos; };
    std::vector<ProbeRef> probes;
    if (rs.getEnvProbesEnabled()) {
        auto probeQuery = em.CreateQuery<ReflectionProbeComponent, Transform>();
        for (auto [entity, probe, xf] : probeQuery) {
            if ((int)probes.size() >= EnvLighting::kMaxProbes) {
                if (!m_envProbeLimitWarned)
                    Debug::Warning("RenderSystem") << "More than " << EnvLighting::kMaxProbes
                        << " reflection probes; the rest are ignored\n";
                m_envProbeLimitWarned = true;
                break;
            }
            probes.push_back({ entity, probe, xf->getPosition() });
        }
    }

    // The images, only when the face list changed (decoding six HDR faces isn't free).
    Hasher facesHash;
    HashSkyFaces(facesHash, sky);
    if (facesHash.h != m_envSkyFacesHash) {
        LoadSkyCubemap(sky);
        m_envSkyFacesHash = facesHash.h;
        m_envSkyReady = false;
    }

    const int res = rs.getEnvMapResolution();
    const int layers = 1 + (int)probes.size();
    if (!m_envPrefiltered || res != m_envRes || layers != m_envLayers)
        AllocateEnvironmentMaps(res, layers);

    Hasher skyHash;
    HashSky(skyHash, sky);
    // Not in the sky itself, but in everything the probes captured under it: a changed sun re-captures them too.
    skyHash.v4(m_cpuDirLight.directionIntensity);
    skyHash.v4(m_cpuDirLight.colorEnabled);

    Hasher setHash;
    setHash.i((int64_t)probes.size());
    for (const ProbeRef& p : probes) {
        setHash.i(p.entity);
        setHash.v3(p.pos);
        setHash.v3(p.probe->boxMin);
        setHash.v3(p.probe->boxMax);
        setHash.f(p.probe->captureNear);
        setHash.f(p.probe->captureFar);
    }

    // Everything below draws offscreen with its own state; what the frame's passes rely on is put back at the end.
    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);

    bool skyRendered = false;
    if (!m_envSkyReady || skyHash.h != m_envSkyHash) {
        RenderSkyToCapture(sky);
        PrefilterCaptureInto(0);
        ProjectCaptureSH(m_envProbeData[0]);
        m_envProbeData[0].boxMin = glm::vec4(0.0f);
        m_envProbeData[0].boxMax = glm::vec4(0.0f);
        m_envProbeData[0].center = glm::vec4(0.0f);   // w = layer 0
        m_envSkyHash = skyHash.h;
        m_envSkyReady = true;
        skyRendered = true;
    }

    // The sky is in every probe's background and lights its contents: a new sky invalidates them all, as does any
    // change in where the probes are.
    if (skyRendered || setHash.h != m_envProbeSetHash) {
        std::fill(m_envProbeReady.begin(), m_envProbeReady.end(), false);
        m_envProbeSetHash = setHash.h;
    }

    int captured = 0;
    for (size_t i = 0; i < probes.size(); ++i) {
        const int layer = (int)i + 1;
        ReflectionProbeComponent& probe = *probes[i].probe;
        if (probe.recapture) {
            m_envProbeReady[layer] = false;
            probe.recapture = false;
        }

        if (!m_envProbeReady[layer] && captured < kProbesPerFrame) {
            CaptureProbeScene(meshQuery, sky, probes[i].pos,
                std::max(probe.captureNear, 0.01f), std::max(probe.captureFar, probe.captureNear + 1.0f));
            PrefilterCaptureInto(layer);
            ProjectCaptureSH(m_envProbeData[layer]);
            m_envProbeReady[layer] = true;
            ++captured;
        }

        GPUEnvProbe& g = m_envProbeData[layer];
        if (m_envProbeReady[layer]) {
            g.boxMin = glm::vec4(glm::min(probe.boxMin, probe.boxMax), std::max(probe.blendDistance, 1e-3f));
            g.boxMax = glm::vec4(glm::max(probe.boxMin, probe.boxMax), probe.parallaxCorrection ? 1.0f : 0.0f);
            const bool ownParallax = glm::all(glm::lessThan(probe.parallaxBoxMin, probe.parallaxBoxMax));
            g.parallaxMin = ownParallax ? glm::vec4(probe.parallaxBoxMin, 0.0f) : glm::vec4(glm::vec3(g.boxMin), 0.0f);
            g.parallaxMax = ownParallax ? glm::vec4(probe.parallaxBoxMax, 0.0f) : glm::vec4(glm::vec3(g.boxMax), 0.0f);
        } else {
            // Not captured yet: an inside-out box, weight 0 everywhere, so the sky stands in until it is.
            g.boxMin = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            g.boxMax = glm::vec4(-1.0f, -1.0f, -1.0f, 0.0f);
            g.parallaxMin = g.parallaxMax = glm::vec4(0.0f);
        }
        g.center = glm::vec4(probes[i].pos, (float)layer);
    }
    m_envProbeCount = (int)probes.size();

    if (captured > 0)
        Debug::Info("RenderSystem") << "Captured " << captured << " reflection probe(s)\n";

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_envProbeSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, layers * sizeof(GPUEnvProbe), m_envProbeData.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    if (blendWasOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

void RenderSystem::DrawSkyFace(const EnvironmentSkyComponent& sky, int face)
{
    glUseProgram(m_envSkyShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envSkySource);
    auto loc = [this](const char* n) { return glGetUniformLocation(m_envSkyShader, n); };
    glUniform1i(loc("uSky"), 0);
    glUniform1i(loc("uHasSky"), m_envSkySource ? 1 : 0);
    glUniform1i(loc("uFace"), face);
    glUniform1i(loc("uSize"), m_envRes);
    const glm::mat3 worldToSky = WorldToSkyRotation(sky);
    glUniformMatrix3fv(loc("uWorldToSky"), 1, GL_FALSE, glm::value_ptr(worldToSky));
    glUniform1f(loc("uIntensity"), std::max(sky.intensity, 0.0f));
    glUniform1f(loc("uLod"), (m_envSkySourceSize > m_envRes && m_envRes > 0)
        ? std::log2(float(m_envSkySourceSize) / float(m_envRes)) : 0.0f);

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void RenderSystem::RenderSkyToCapture(const EnvironmentSkyComponent& sky)
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_envFBO);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    glViewport(0, 0, m_envRes, m_envRes);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    for (int f = 0; f < 6; ++f) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, m_envCaptureCube, 0);
        DrawSkyFace(sky, f);
    }
    glDepthMask(GL_TRUE);
}

// An image asset decoded to linear float RGB (.hdr as is, anything else as sRGB). False (and logged) if the asset is
// missing or can't be decoded; `what` names it in the message.
static bool DecodeImageAsset(const std::string& name, const std::string& what, int& w, int& h, std::vector<float>& out)
{
    out.clear();
    w = h = 0;
    auto blob = name.empty() ? std::nullopt : AssetManager::instance().loadAsset<BlobData>(name);
    if (!blob || !blob->bytes || blob->bytes->empty()) {
        Debug::Error("RenderSystem") << "Sky: " << what << " ('" << name
            << "') not found; is it listed in the scene's .ntfg?\n";
        if (blob) AssetManager::instance().unloadAsset<BlobData>(name);
        return false;
    }

    const stbi_uc* data = blob->bytes->data();
    const int len = (int)blob->bytes->size();
    int comp = 0;
    if (stbi_is_hdr_from_memory(data, len)) {
        float* px = stbi_loadf_from_memory(data, len, &w, &h, &comp, 3);
        if (px) { out.assign(px, px + (size_t)w * h * 3); stbi_image_free(px); }
    } else {
        stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &comp, 3);
        if (px) {
            out.resize((size_t)w * h * 3);
            for (size_t k = 0; k < out.size(); ++k) out[k] = SrgbToLinear(px[k]);
            stbi_image_free(px);
        }
    }
    AssetManager::instance().unloadAsset<BlobData>(name);

    if (out.empty()) {
        Debug::Error("RenderSystem") << "Sky: can't decode " << what << " ('" << name << "'): "
            << stbi_failure_reason() << "\n";
        return false;
    }
    return true;
}

// Empty RGBA16F cube map with a full mip chain, linear filtering: what both sky paths fill.
static GLuint CreateSkyCube(int size)
{
    const int levels = 1 + (int)std::floor(std::log2((float)size));
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, levels, GL_RGBA16F, size, size);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    return tex;
}

// The sky's images -> m_envSkySource (RGBA16F cube map with mips): the six faces uploaded as they are, or the
// panorama converted on the GPU. Any failure (missing asset, undecodable, wrong sizes) leaves no source: the sky
// renders black and the error says which image.
bool RenderSystem::LoadSkyCubemap(const EnvironmentSkyComponent& sky)
{
    if (m_envSkySource) glDeleteTextures(1, &m_envSkySource);
    m_envSkySource = 0;
    m_envSkySourceSize = 0;

    if (!sky.equirect.empty())
        return LoadSkyEquirect(sky.equirect);

    bool anyFace = false;
    for (const std::string& f : sky.faces) anyFace = anyFace || !f.empty();
    if (!anyFace) return false;   // no sky set: black, on purpose

    std::vector<float> faces[6];
    int size = 0;
    for (int i = 0; i < 6; ++i) {
        int w = 0, h = 0;
        if (!DecodeImageAsset(sky.faces[i], "cube map face " + std::to_string(i), w, h, faces[i]))
            return false;
        if (w != h || (i > 0 && w != size)) {
            Debug::Error("RenderSystem") << "Sky: cube map face " << i << " ('" << sky.faces[i] << "') is " << w << "x"
                << h << "; every face must be square and the same size\n";
            return false;
        }
        size = w;
    }

    // First image row = top of the face = GL's first row (t = 0) for a cube map face: uploaded as is, no flip.
    m_envSkySource = CreateSkyCube(size);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (int i = 0; i < 6; ++i)
        glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, 0, 0, size, size, GL_RGB, GL_FLOAT, faces[i].data());
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    m_envSkySourceSize = size;

    Debug::Info("RenderSystem") << "Sky cube map loaded: " << size << "x" << size << " ('" << sky.faces[0] << "'...)\n";
    return true;
}

// Equirectangular panorama -> cube map: uploaded as a 2D texture (first row = top = t 0, no flip), then every texel of
// every face looks its direction up in it (m_envEquirectShader). Faces are width / 4 wide, which keeps about one
// panorama texel per cube texel at the horizon, capped at 2048.
bool RenderSystem::LoadSkyEquirect(const std::string& name)
{
    int w = 0, h = 0;
    std::vector<float> pixels;
    if (!DecodeImageAsset(name, "panorama", w, h, pixels))
        return false;
    if (w < 8 || h < 4)
        return false;
    if (std::abs(w - 2 * h) > 2)
        Debug::Warning("RenderSystem") << "Sky: panorama '" << name << "' is " << w << "x" << h
            << ", not 2:1; it will look stretched\n";
    if (!m_envEquirectShader || !m_envFBO || !m_quadVAO) return false;

    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    if (w > maxTex || h > maxTex) {
        Debug::Error("RenderSystem") << "Sky: panorama '" << name << "' (" << w << "x" << h
            << ") exceeds this GPU's max texture size " << maxTex << "\n";
        return false;
    }

    // Mips for the bottom/top rows, which squeeze a whole row into a few cube texels near the poles.
    GLuint pano = 0;
    const int panoLevels = 1 + (int)std::floor(std::log2((float)std::max(w, h)));
    glGenTextures(1, &pano);
    glBindTexture(GL_TEXTURE_2D, pano);
    glTexStorage2D(GL_TEXTURE_2D, panoLevels, GL_RGBA16F, w, h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_FLOAT, pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);         // longitude wraps around
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::vector<float>().swap(pixels);

    const int size = std::clamp(w / 4, 8, 2048);
    m_envSkySource = CreateSkyCube(size);

    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    const GLboolean depthWasOn = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, m_envFBO);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    glViewport(0, 0, size, size);
    glUseProgram(m_envEquirectShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pano);
    glUniform1i(glGetUniformLocation(m_envEquirectShader, "uPanorama"), 0);
    glUniform1i(glGetUniformLocation(m_envEquirectShader, "uSize"), size);
    // Panorama texels per cube texel along a row is ~w / (4 * size): read the mip that matches (0 unless capped).
    glUniform1f(glGetUniformLocation(m_envEquirectShader, "uLod"), std::max(0.0f, std::log2(float(w) / float(4 * size))));
    const GLint faceLoc = glGetUniformLocation(m_envEquirectShader, "uFace");

    glBindVertexArray(m_quadVAO);
    for (int f = 0; f < 6; ++f) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, m_envSkySource, 0);
        glUniform1i(faceLoc, f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindVertexArray(0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(1, &pano);

    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envSkySource);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    m_envSkySourceSize = size;

    if (blendWasOn) glEnable(GL_BLEND);
    if (depthWasOn) glEnable(GL_DEPTH_TEST);
    glViewport(0, 0, m_screenW, m_screenH);

    Debug::Info("RenderSystem") << "Sky panorama loaded: '" << name << "' " << w << "x" << h << " -> cube map "
        << size << "x" << size << "\n";
    return true;
}

void RenderSystem::CaptureProbeScene(EntityManager::Query<MeshComponent, Transform>& meshQuery,
    const EnvironmentSkyComponent& sky, const glm::vec3& pos, float nearPlane, float farPlane)
{
    const auto& rs = RenderSettings::instance();
    const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);

    for (int f = 0; f < 6; ++f) {
        const glm::mat4 view = glm::lookAt(pos, pos + kFaceDir[f], kFaceUp[f]);

        // Directional shadows fitted to this face's frustum (the frame's cascades follow the camera, they'd cover
        // little of what the probe sees). Its own FBO and viewport: before binding the capture target.
        if (rs.getDirShadowsEnabled())
            DirShadowPass(meshQuery, view, projection, nearPlane, farPlane, true);

        glBindFramebuffer(GL_FRAMEBUFFER, m_envFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, m_envCaptureCube, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_envCaptureDepth);
        if (f == 0 && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            Debug::Error("RenderSystem") << "Reflection probe capture FBO incomplete\n";
        glViewport(0, 0, m_envRes, m_envRes);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);

        // Background: the sky wherever no geometry lands.
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        DrawSkyFace(sky, f);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);

        // The static scene, lit like the frame (minus SSAO and point light shadows, plus the sky's environment).
        BindLightingInputs(true);
        for (auto [entity, meshC, transform] : meshQuery) {
            if (!meshC->enabled || !meshC->mesh || meshC->additive || !meshC->reflectionProbeVisible) continue;
            if (Material* mat = meshC->mesh->getMaterial())
                SetLightingUniforms(mat, pos, true);
            meshC->mesh->bindMaterial(transform->getModelMatrix(), view, projection);
            meshC->mesh->draw();
        }
        UnbindLightingInputs();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderSystem::PrefilterCaptureInto(int layer)
{
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCaptureCube);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    glBindFramebuffer(GL_FRAMEBUFFER, m_envFBO);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    glUseProgram(m_envPrefilterShader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCaptureCube);
    glUniform1i(glGetUniformLocation(m_envPrefilterShader, "uSource"), 0);
    glUniform1f(glGetUniformLocation(m_envPrefilterShader, "uSourceSize"), (float)m_envRes);
    glUniform1i(glGetUniformLocation(m_envPrefilterShader, "uSampleCount"), kPrefilterSamples);
    const GLint faceLoc = glGetUniformLocation(m_envPrefilterShader, "uFace");
    const GLint sizeLoc = glGetUniformLocation(m_envPrefilterShader, "uTargetSize");
    const GLint roughLoc = glGetUniformLocation(m_envPrefilterShader, "uRoughness");

    glBindVertexArray(m_quadVAO);
    for (int mip = 0; mip < m_envMips; ++mip) {
        const int size = std::max(1, m_envRes >> mip);
        glViewport(0, 0, size, size);
        glUniform1i(sizeLoc, size);
        glUniform1f(roughLoc, m_envMips > 1 ? float(mip) / float(m_envMips - 1) : 0.0f);
        for (int f = 0; f < 6; ++f) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_envPrefiltered, mip, layer * 6 + f);
            glUniform1i(faceLoc, f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    glBindVertexArray(0);

    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDepthMask(GL_TRUE);
}

// Order-2 SH of the capture's radiance (read back from its 32x32 mip), convolved with the clamped cosine and divided by
// pi so the shader's sum is directly the outgoing radiance of a white Lambert surface: sh_k = A_l / pi * K_k * L_k,
// with L_k = sum(radiance * K_k * poly_k * solid angle) and the shader evaluating sum(sh_k * poly_k(N)).
void RenderSystem::ProjectCaptureSH(GPUEnvProbe& out)
{
    int level = 0, n = m_envRes;
    while (n > kSHSampleSize) { n >>= 1; ++level; }

    // Basis constants K_k and cosine-lobe factors A_l / pi, in the shader's coefficient order:
    // 1, y, z, x, xy, yz, 3z^2 - 1, xz, x^2 - y^2.
    static const double K[9] = { 0.282095, 0.488603, 0.488603, 0.488603, 1.092548, 1.092548, 0.315392, 1.092548, 0.546274 };
    static const double A[9] = { 1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25 };

    std::vector<float> pixels((size_t)n * n * 4);
    double acc[9][3] = {};
    double weightSum = 0.0;

    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCaptureCube);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int f = 0; f < 6; ++f) {
        glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, level, GL_RGBA, GL_FLOAT, pixels.data());
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const float u = (x + 0.5f) / n * 2.0f - 1.0f;
                const float v = (y + 0.5f) / n * 2.0f - 1.0f;
                const glm::vec3 d = glm::normalize(FaceDir(f, u, v));
                // Solid angle of the texel, up to a constant factor (normalised below).
                const double w = 1.0 / std::pow(1.0 + u * u + v * v, 1.5);

                const float* p = &pixels[((size_t)y * n + x) * 4];
                double rgb[3];
                for (int c = 0; c < 3; ++c)
                    rgb[c] = std::isfinite(p[c]) ? std::max(0.0, (double)p[c]) : 0.0;

                const double poly[9] = {
                    1.0, d.y, d.z, d.x, d.x * d.y, d.y * d.z, 3.0 * d.z * d.z - 1.0, d.x * d.z, d.x * d.x - d.y * d.y,
                };
                for (int k = 0; k < 9; ++k)
                    for (int c = 0; c < 3; ++c)
                        acc[k][c] += rgb[c] * poly[k] * w;
                weightSum += w;
            }
        }
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    const double norm = 4.0 * 3.14159265358979 / std::max(weightSum, 1e-9);
    for (int k = 0; k < 9; ++k) {
        const double s = A[k] * K[k] * K[k] * norm;
        out.sh[k] = glm::vec4((float)(acc[k][0] * s), (float)(acc[k][1] * s), (float)(acc[k][2] * s), 0.0f);
    }
}
