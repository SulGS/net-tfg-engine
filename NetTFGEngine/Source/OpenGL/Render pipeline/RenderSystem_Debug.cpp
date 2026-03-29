#include "RenderSystem.hpp"

#include "stb_image_write.h"

#include <filesystem>
#include <vector>
#include <string>
#include <ctime>

// =====================================================
//  Internal helpers  (file-scope only)
// =====================================================
namespace {

    static constexpr const char* kDumpDir = "Render";

    // ------------------------------------------------------------------
    //  WritePNG
    // ------------------------------------------------------------------
    static void WritePNG(const std::string& path,
        int w, int h,
        const std::vector<uint8_t>& rgb)
    {
        const uint8_t* lastRow = rgb.data() + static_cast<ptrdiff_t>(w * 3) * (h - 1);
        if (!stbi_write_png(path.c_str(), w, h, 3, lastRow, -w * 3))
            Debug::Error("RenderSystem::DumpBuffers") << "stbi_write_png failed: " << path << "\n";
    }

    // ------------------------------------------------------------------
    //  ReinhardTonemap + gamma
    // ------------------------------------------------------------------
    static uint8_t TonemapChannel(float v, float exposure, float invGamma)
    {
        v *= exposure;
        v = v / (v + 1.0f);
        if (v < 0.0f) v = 0.0f;
        v = glm::pow(v, invGamma);
        if (v > 1.0f) v = 1.0f;
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
    }

    // ------------------------------------------------------------------
    //  DumpTexture2D_RGBA16F
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    //  DumpTexture2D_Depth32F
    //  Auto-ranges min/max for maximum readability.
    //  Pixels at depth == 1.0 (cleared background) → black.
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    //  DumpTexture2D_DirShadow
    //  Reads the directional light shadow map (DEPTH32F, comparison mode
    //  set on the sampler).  To read the raw depth data back we must
    //  temporarily disable the comparison mode so glGetTexImage returns
    //  depth values rather than comparison results, then restore it.
    //  Visualised with auto-ranging so the depth gradient is always visible.
    // ------------------------------------------------------------------
    static void DumpTexture2D_DirShadow(GLuint tex,
        int res,
        const std::string& path)
    {
        // Temporarily disable comparison mode for the raw readback.
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

        const int nPix = res * res;
        std::vector<float> depth(nPix);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());

        // Restore comparison mode.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glBindTexture(GL_TEXTURE_2D, 0);

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

    // ------------------------------------------------------------------
    //  DumpCubeArrayFace
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    //  DumpTexture2D_RGBA8
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    //  DumpDefaultFramebuffer
    // ------------------------------------------------------------------
    static void DumpDefaultFramebuffer(int w, int h, const std::string& path)
    {
        const int nPix = w * h;
        std::vector<uint8_t> pixels(nPix * 3);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

        WritePNG(path, w, h, pixels);
    }

    // ------------------------------------------------------------------
    //  DumpTexture2D_ViewNormals
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    //  DumpTexture2D_GreyscaleR
    // ------------------------------------------------------------------
    static void DumpTexture2D_GreyscaleR(GLuint tex, int w, int h,
        const std::string& path)
    {
        const int nPix = w * h;
        std::vector<float> pixels(nPix * 4);
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            float v = pixels[i * 4 + 0];
            v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            auto b = static_cast<uint8_t>(v * 255.f + 0.5f);
            rgb[i * 3 + 0] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = b;
        }
        WritePNG(path, w, h, rgb);
    }

} // anonymous namespace

// =====================================================
//  DumpBuffers
//
//  Files written (all PNG, 8-bit RGB):
//    hdr_color.png            — HDR colour, Reinhard-tonemapped for viewing
//    depth.png                — Scene depth, auto-ranged greyscale
//    gbuffer_normal.png       — View-space normals as RGB
//    gbuffer_roughness.png    — Perceptual roughness as greyscale
//    gbuffer_metalness.png    — Metalness as greyscale
//    bloom_thresh.png         — Bloom threshold pass output
//    bloom_result.png         — Final blurred bloom texture
//    ldr_color.png            — Post-tonemap LDR colour (pre-FXAA)
//    final_output.png         — Exact screen pixels (post-FXAA)
//    shadow_L{n}_F{f}_{dir}.png — Point light shadow cubemap faces
//    dir_shadow.png           — Directional light orthographic shadow map
// =====================================================
void RenderSystem::DumpBuffers() const
{
    // ---- Build timestamped subfolder ----
    std::time_t now = std::time(nullptr);
    std::tm     tm = {};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char tsBuf[32];
    std::strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d_%H-%M-%S", &tm);
    std::string dumpDir = std::string(kDumpDir) + "/" + tsBuf;

    std::error_code ec;
    std::filesystem::create_directories(dumpDir, ec);
    if (ec) {
        Debug::Error("RenderSystem::DumpBuffers")
            << "Failed to create directory '" << dumpDir
            << "': " << ec.message() << "\n";
        return;
    }

    const auto& rs = RenderSettings::instance();
    const float exposure = rs.getExposure();
    const float gamma = rs.getGamma();

    auto path = [&](const std::string& name) -> std::string {
        return dumpDir + "/" + name;
        };

    // ---- 1. HDR colour ----
    if (m_hdrColorTex) {
        DumpTexture2D_RGBA16F(m_hdrColorTex, m_screenW, m_screenH,
            exposure, gamma, path("hdr_color.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved hdr_color.png\n";
    }

    // ---- 2. Scene depth ----
    if (m_hdrDepthTex) {
        DumpTexture2D_Depth32F(m_hdrDepthTex, m_screenW, m_screenH,
            path("depth.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved depth.png\n";
    }

    // ---- 3. GBuffer normals ----
    if (m_gbufferNormalTex) {
        DumpTexture2D_ViewNormals(m_gbufferNormalTex, m_screenW, m_screenH,
            path("gbuffer_normal.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved gbuffer_normal.png\n";
    }

    // ---- 3b. GBuffer roughness ----
    if (m_gbufferRoughnessTex) {
        DumpTexture2D_GreyscaleR(m_gbufferRoughnessTex, m_screenW, m_screenH,
            path("gbuffer_roughness.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved gbuffer_roughness.png\n";
    }

    // ---- 3c. GBuffer metalness ----
    if (m_gbufferMetalnessTex) {
        DumpTexture2D_GreyscaleR(m_gbufferMetalnessTex, m_screenW, m_screenH,
            path("gbuffer_metalness.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved gbuffer_metalness.png\n";
    }

    // ---- 4. Bloom threshold ----
    if (m_bloomThreshTex) {
        DumpTexture2D_RGBA16F(m_bloomThreshTex, m_screenW, m_screenH,
            exposure, gamma, path("bloom_thresh.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved bloom_thresh.png\n";
    }

    // ---- 5. Bloom result ----
    if (m_bloomPingTex) {
        const int bW = std::max(1, m_screenW / 2);
        const int bH = std::max(1, m_screenH / 2);
        DumpTexture2D_RGBA16F(m_bloomPingTex, bW, bH,
            1.0f, 1.0f, path("bloom_result.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved bloom_result.png\n";
    }

    // ---- 6. LDR colour (post-tonemap, pre-FXAA) ----
    if (m_ldrTex) {
        DumpTexture2D_RGBA8(m_ldrTex, m_screenW, m_screenH, path("ldr_color.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved ldr_color.png\n";
    }

    // ---- 7. Final output ----
    DumpDefaultFramebuffer(m_screenW, m_screenH, path("final_output.png"));
    Debug::Info("RenderSystem::DumpBuffers") << "Saved final_output.png\n";

    // ---- 8. Point light shadow cubemap faces ----
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

    // ---- 9. Directional light shadow map ----
    // Only dump when the texture was actually rendered into (FBO was used this
    // frame).  We check m_dirShadowTex rather than a separate "was rendered"
    // flag — if it's non-zero Init() created it, and DirShadowPass() would
    // have filled it if a directional light exists and shadows are enabled.
    if (m_dirShadowTex) {
        const int dirRes = RenderSettings::instance().getDirShadowResolution();
        DumpTexture2D_DirShadow(m_dirShadowTex, dirRes, path("dir_shadow.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved dir_shadow.png\n";
    }

    Debug::Info("RenderSystem::DumpBuffers")
        << "Buffer dump complete -> '" << dumpDir << "/'\n";
}