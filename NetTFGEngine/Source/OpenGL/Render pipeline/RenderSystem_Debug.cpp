#include "RenderSystem.hpp"

#include "stb_image_write.h"

#include <filesystem>
#include <vector>
#include <string>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <algorithm>

// Internal helpers (file-scope only)
namespace {

    static constexpr const char* kDumpDir = "Render";

    static void WritePNG(const std::string& path,
        int w, int h,
        const std::vector<uint8_t>& rgb)
    {
        const uint8_t* lastRow = rgb.data() + static_cast<ptrdiff_t>(w * 3) * (h - 1);
        if (!stbi_write_png(path.c_str(), w, h, 3, lastRow, -w * 3))
            Debug::Error("RenderSystem::DumpBuffers") << "stbi_write_png failed: " << path << "\n";
    }

    // Reinhard tonemap + gamma
    static uint8_t TonemapChannel(float v, float exposure, float invGamma)
    {
        v *= exposure;
        v = v / (v + 1.0f);
        if (v < 0.0f) v = 0.0f;
        v = glm::pow(v, invGamma);
        if (v > 1.0f) v = 1.0f;
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
    }

    static void DumpTexture2D_RGBA16F(GLuint tex,
        int w, int h,
        float exposure, float gamma,
        const std::string& path)
    {
        const int   nPix = w * h;
        std::vector<float> pixels(nPix * 4);

        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        const float invGamma = 1.0f / gamma;
        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            rgb[i * 3 + 0] = TonemapChannel(pixels[i * 4 + 0], exposure, invGamma);
            rgb[i * 3 + 1] = TonemapChannel(pixels[i * 4 + 1], exposure, invGamma);
            rgb[i * 3 + 2] = TonemapChannel(pixels[i * 4 + 2], exposure, invGamma);
        }

        WritePNG(path, w, h, rgb);
    }

    // Auto-ranges min/max for readability; pixels at depth == 1.0 (cleared background) render black.
    static void DumpTexture2D_Depth32F(GLuint tex,
        int w, int h,
        const std::string& path)
    {
        const int nPix = w * h;
        std::vector<float> depth(nPix);

        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        float dMin = 1.0f, dMax = 0.0f;
        for (int i = 0; i < nPix; ++i) {
            float z = depth[i];
            if (z < 1.0f) {
                if (z < dMin) dMin = z;
                if (z > dMax) dMax = z;
            }
        }
        const float range = (dMax > dMin) ? (dMax - dMin) : 1.0f;
        if (dMax <= dMin) { dMin = 0.0f; }

        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            float z = depth[i];
            float norm = (z >= 1.0f) ? 0.0f : (z - dMin) / range;
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
            auto  byte = static_cast<uint8_t>(norm * 255.0f + 0.5f);
            rgb[i * 3 + 0] = byte;
            rgb[i * 3 + 1] = byte;
            rgb[i * 3 + 2] = byte;
        }

        WritePNG(path, w, h, rgb);
    }

    // One cascade (layer) of the directional shadow array, raw depth visualised with auto-ranging.
    static void DumpTexture2D_DirShadow(GLuint tex,
        int res, int layers, int layer,
        const std::string& path)
    {
        const int nPix = res * res;
        std::vector<float> all(static_cast<size_t>(nPix) * layers);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT, GL_FLOAT, all.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        const std::vector<float> depth(all.begin() + static_cast<size_t>(nPix) * layer,
                                       all.begin() + static_cast<size_t>(nPix) * (layer + 1));

        // Auto-range (same logic as DumpTexture2D_Depth32F).
        float dMin = 1.0f, dMax = 0.0f;
        for (int i = 0; i < nPix; ++i) {
            float z = depth[i];
            if (z < 1.0f) {
                if (z < dMin) dMin = z;
                if (z > dMax) dMax = z;
            }
        }
        const float range = (dMax > dMin) ? (dMax - dMin) : 1.0f;
        if (dMax <= dMin) { dMin = 0.0f; }

        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            float z = depth[i];
            float norm = (z >= 1.0f) ? 0.0f : (z - dMin) / range;
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
            auto  byte = static_cast<uint8_t>(norm * 255.0f + 0.5f);
            rgb[i * 3 + 0] = byte;
            rgb[i * 3 + 1] = byte;
            rgb[i * 3 + 2] = byte;
        }

        WritePNG(path, res, res, rgb);
    }

    static void DumpCubeArrayFace(GLuint tex,
        int res,
        int layer,
        const std::string& path)
    {
        const int nPix = res * res;
        std::vector<float> depth(nPix);

#if defined(GL_VERSION_4_5)
        glGetTextureSubImage(tex,
            /*level*/  0,
            /*xoff*/   0, /*yoff*/ 0, /*zoff*/ layer,
            /*w*/      res, /*h*/  res, /*d*/  1,
            GL_DEPTH_COMPONENT, GL_FLOAT,
            static_cast<GLsizei>(nPix * sizeof(float)),
            depth.data());
#else
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, tex, 0, layer);
        glReadPixels(0, 0, res, res, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
#endif

        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            float v = depth[i];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            auto byte = static_cast<uint8_t>(v * 255.0f + 0.5f);
            rgb[i * 3 + 0] = byte;
            rgb[i * 3 + 1] = byte;
            rgb[i * 3 + 2] = byte;
        }

        WritePNG(path, res, res, rgb);
    }

    static void DumpTexture2D_RGBA8(GLuint tex,
        int w, int h,
        const std::string& path)
    {
        const int nPix = w * h;
        std::vector<uint8_t> pixels(nPix * 4);

        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            rgb[i * 3 + 0] = pixels[i * 4 + 0];
            rgb[i * 3 + 1] = pixels[i * 4 + 1];
            rgb[i * 3 + 2] = pixels[i * 4 + 2];
        }
        WritePNG(path, w, h, rgb);
    }

    static void DumpDefaultFramebuffer(int w, int h, const std::string& path)
    {
        const int nPix = w * h;
        std::vector<uint8_t> pixels(nPix * 3);

        // Tightly packed RGB rows (the default 4-byte alignment pads them when w * 3 isn't a multiple of 4).
        GLint prevPack = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

        glPixelStorei(GL_PACK_ALIGNMENT, prevPack);

        WritePNG(path, w, h, pixels);
    }

    static void DumpTexture2D_ViewNormals(GLuint tex, int w, int h,
        const std::string& path)
    {
        const int nPix = w * h;
        std::vector<float> pixels(nPix * 4);
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            auto remap = [](float v) -> uint8_t {
                v = v * 0.5f + 0.5f;
                v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                return static_cast<uint8_t>(v * 255.f + 0.5f);
                };
            rgb[i * 3 + 0] = remap(pixels[i * 4 + 0]);
            rgb[i * 3 + 1] = remap(pixels[i * 4 + 1]);
            rgb[i * 3 + 2] = remap(pixels[i * 4 + 2]);
        }
        WritePNG(path, w, h, rgb);
    }

    static std::vector<float> ReadRGBA32F(GLuint tex, int w, int h)
    {
        std::vector<float> pixels(static_cast<size_t>(w) * h * 4);
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        return pixels;
    }

    // The scene as the tonemap shader sees it: hdr + bloom * strength, bloom (bw x bh) sampled like texture() with
    // GL_LINEAR + CLAMP_TO_EDGE at the full-res pixel centre, inf clamped / NaN zeroed as the shader does. Then
    // tonemapped like the other HDR dumps.
    static void DumpTexture2D_HDRWithBloom(GLuint hdrTex, int w, int h,
        GLuint bloomTex, int bw, int bh, float strength,
        float exposure, float gamma, const std::string& path)
    {
        const std::vector<float> hdr = ReadRGBA32F(hdrTex, w, h);
        const std::vector<float> bloom = ReadRGBA32F(bloomTex, bw, bh);

        auto bloomAt = [&](int x, int y, int c) {
            return bloom[(static_cast<size_t>(std::clamp(y, 0, bh - 1)) * bw + std::clamp(x, 0, bw - 1)) * 4 + c];
            };

        const float invGamma = 1.0f / gamma;
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            const float by = (y + 0.5f) / h * bh - 0.5f;
            const int   y0 = static_cast<int>(std::floor(by));
            const float fy = by - y0;
            for (int x = 0; x < w; ++x) {
                const float bx = (x + 0.5f) / w * bw - 0.5f;
                const int   x0 = static_cast<int>(std::floor(bx));
                const float fx = bx - x0;

                const size_t i = static_cast<size_t>(y) * w + x;
                float v[3];
                bool  nan = false;
                for (int c = 0; c < 3; ++c) {
                    const float top = bloomAt(x0, y0, c) * (1.0f - fx) + bloomAt(x0 + 1, y0, c) * fx;
                    const float bot = bloomAt(x0, y0 + 1, c) * (1.0f - fx) + bloomAt(x0 + 1, y0 + 1, c) * fx;
                    v[c] = hdr[i * 4 + c] + (top * (1.0f - fy) + bot * fy) * strength;
                    nan |= std::isnan(v[c]);
                }
                for (int c = 0; c < 3; ++c)
                    rgb[i * 3 + c] = TonemapChannel(nan ? 0.0f : std::min(v[c], 60000.0f), exposure, invGamma);
            }
        }
        WritePNG(path, w, h, rgb);
    }

    // One channel (0 = R ... 3 = A) as greyscale. w/h must be the texture's own size.
    static void DumpTexture2D_GreyscaleR(GLuint tex, int w, int h,
        const std::string& path, int channel = 0)
    {
        const int nPix = w * h;
        std::vector<float> pixels(nPix * 4);
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            float v = pixels[i * 4 + channel];
            v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            auto b = static_cast<uint8_t>(v * 255.f + 0.5f);
            rgb[i * 3 + 0] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = b;
        }
        WritePNG(path, w, h, rgb);
    }

} // anonymous namespace

std::string RenderSystem::DumpTimestamp()
{
    std::time_t now = std::time(nullptr);
    std::tm     tm = {};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char tsBuf[32];
    std::strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d_%H-%M-%S", &tm);
    return tsBuf;
}

// Creates Render/<timestamp>/ and returns it, or an empty string on failure.
std::string RenderSystem::MakeDumpDir()
{
    std::string dumpDir = std::string(kDumpDir) + "/" + DumpTimestamp();

    std::error_code ec;
    std::filesystem::create_directories(dumpDir, ec);
    if (ec) {
        Debug::Error("RenderSystem::DumpBuffers")
            << "Failed to create directory '" << dumpDir
            << "': " << ec.message() << "\n";
        return {};
    }
    return dumpDir;
}

void RenderSystem::DumpStage(const char* name, StageImage kind, GLuint tex, int w, int h)
{
    if (m_stageDumpDir.empty()) return;
    if (kind != StageImage::Window && !tex) return;

    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "stage_%02d_", m_stageDumpIndex++);
    const std::string path = m_stageDumpDir + "/" + prefix + name + ".png";

    // The helpers below rebind GL_TEXTURE_2D on the active unit (and the window FBO): put back what the passes left.
    GLint prevTex = 0, prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

    const auto& rs = RenderSettings::instance();
    switch (kind) {
    case StageImage::HDR:     DumpTexture2D_RGBA16F(tex, w, h, rs.getExposure(), rs.getGamma(), path); break;
    case StageImage::HDRBloom:
        DumpTexture2D_HDRWithBloom(tex, w, h, m_bloomPingTex, std::max(1, m_screenW / 2), std::max(1, m_screenH / 2),
            rs.getBloomStrength(), rs.getExposure(), rs.getGamma(), path);
        break;
    case StageImage::LDR:     DumpTexture2D_RGBA8(tex, w, h, path); break;
    case StageImage::Window:  DumpDefaultFramebuffer(m_outputW, m_outputH, path); break;
    case StageImage::Grey:    DumpTexture2D_GreyscaleR(tex, w, h, path); break;
    case StageImage::Normals: DumpTexture2D_ViewNormals(tex, w, h, path); break;
    case StageImage::Depth:   DumpTexture2D_Depth32F(tex, w, h, path); break;
    }

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));

    Debug::Info("RenderSystem::DumpBuffers") << "Saved " << prefix << name << ".png\n";
}

// The window's back buffer as Render/<timestamp>.png (_2, _3... when several land in the same second).
void RenderSystem::DumpFinalFrame() const
{
    std::error_code ec;
    std::filesystem::create_directories(kDumpDir, ec);
    if (ec) {
        Debug::Error("RenderSystem::DumpFinalFrame")
            << "Failed to create directory '" << kDumpDir << "': " << ec.message() << "\n";
        return;
    }

    const std::string base = std::string(kDumpDir) + "/" + DumpTimestamp();
    std::string path = base + ".png";
    for (int n = 2; std::filesystem::exists(path); ++n)
        path = base + "_" + std::to_string(n) + ".png";

    GLint prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    DumpDefaultFramebuffer(m_outputW, m_outputH, path);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));

    Debug::Info("RenderSystem::DumpFinalFrame") << "Saved " << path << "\n";
}

// Dumps every render target (HDR/depth/GBuffer/bloom/LDR/final/shadows) as 8-bit PNGs into dumpDir (a new timestamped
// subfolder when empty).
void RenderSystem::DumpBuffers(std::string dumpDir) const
{
    if (dumpDir.empty()) dumpDir = MakeDumpDir();
    if (dumpDir.empty()) return;

    const auto& rs = RenderSettings::instance();
    const float exposure = rs.getExposure();
    const float gamma = rs.getGamma();

    auto path = [&](const std::string& name) -> std::string {
        return dumpDir + "/" + name;
        };

    // 1. HDR colour
    if (m_hdrColorTex) {
        DumpTexture2D_RGBA16F(m_hdrColorTex, m_screenW, m_screenH,
            exposure, gamma, path("hdr_color.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved hdr_color.png\n";
    }

    // 2. Scene depth
    if (m_hdrDepthTex) {
        DumpTexture2D_Depth32F(m_hdrDepthTex, m_screenW, m_screenH,
            path("depth.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved depth.png\n";
    }

    // 3. GBuffer normals
    if (m_gbufferNormalTex) {
        DumpTexture2D_ViewNormals(m_gbufferNormalTex, m_screenW, m_screenH,
            path("gbuffer_normal.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved gbuffer_normal.png\n";
    }

    // 3b. GBuffer roughness (alpha of the normal target)
    if (m_gbufferNormalTex) {
        DumpTexture2D_GreyscaleR(m_gbufferNormalTex, m_screenW, m_screenH,
            path("gbuffer_roughness.png"), 3);
        Debug::Info("RenderSystem::DumpBuffers") << "Saved gbuffer_roughness.png\n";
    }

    // 3c. GBuffer metalness
    if (m_gbufferMaterialTex) {
        DumpTexture2D_GreyscaleR(m_gbufferMaterialTex, m_screenW, m_screenH,
            path("gbuffer_metalness.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved gbuffer_metalness.png\n";
    }

    // 3d. SSAO (blurred, what the shading pass sampled; at the SSAO resolution scale)
    if (m_ssaoBlurTex) {
        DumpTexture2D_GreyscaleR(m_ssaoBlurTex, m_ssaoW, m_ssaoH, path("ssao.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved ssao.png\n";
    }

    // 4. Bloom threshold
    if (m_bloomThreshTex) {
        DumpTexture2D_RGBA16F(m_bloomThreshTex, m_screenW, m_screenH,
            exposure, gamma, path("bloom_thresh.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved bloom_thresh.png\n";
    }

    // 5. Bloom result
    if (m_bloomPingTex) {
        const int bW = std::max(1, m_screenW / 2);
        const int bH = std::max(1, m_screenH / 2);
        DumpTexture2D_RGBA16F(m_bloomPingTex, bW, bH,
            1.0f, 1.0f, path("bloom_result.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved bloom_result.png\n";
    }

    // 6. LDR colour (post-tonemap, pre-FXAA)
    if (m_ldrTex) {
        DumpTexture2D_RGBA8(m_ldrTex, m_screenW, m_screenH, path("ldr_color.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved ldr_color.png\n";
    }

    // 7. Final output
    DumpDefaultFramebuffer(m_outputW, m_outputH, path("final_output.png"));
    Debug::Info("RenderSystem::DumpBuffers") << "Saved final_output.png\n";

    // 8. Point light shadow cubemap faces
    if (m_shadowCubeArray && m_shadowCount > 0) {
        static constexpr const char* kFaceNames[6] = {
            "pX", "nX", "pY", "nY", "pZ", "nZ"
        };
        for (int light = 0; light < m_shadowCount; ++light) {
            for (int face = 0; face < 6; ++face) {
                int         layer = light * 6 + face;
                std::string name = "shadow_L" + std::to_string(light)
                    + "_F" + std::to_string(face)
                    + "_" + kFaceNames[face] + ".png";
                DumpCubeArrayFace(m_shadowCubeArray, m_shadowRes, layer, path(name));
                Debug::Info("RenderSystem::DumpBuffers") << "Saved " << name << "\n";
            }
        }
    }

    // 9. Directional light shadow cascades (m_dirShadowTex non-zero implies Init() created it and DirShadowPass() filled it, if enabled)
    if (m_dirShadowTex) {
        for (int c = 0; c < m_dirCascadeCount; c++) {
            const std::string name = "dir_shadow_c" + std::to_string(c) + ".png";
            DumpTexture2D_DirShadow(m_dirShadowTex, m_dirShadowRes, m_dirCascadeCount, c, path(name));
            Debug::Info("RenderSystem::DumpBuffers") << "Saved " << name << "\n";
        }
    }

    Debug::Info("RenderSystem::DumpBuffers")
        << "Buffer dump complete -> '" << dumpDir << "/'\n";
}