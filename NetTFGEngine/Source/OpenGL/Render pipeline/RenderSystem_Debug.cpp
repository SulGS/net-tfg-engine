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
    //  Writes an 8-bit RGB PNG via stb_image_write.
    // ------------------------------------------------------------------
    static void WritePNG(const std::string& path,
        int w, int h,
        const std::vector<uint8_t>& rgb)
    {
        const uint8_t* lastRow = rgb.data() + static_cast<ptrdiff_t>(w * 3) * (h - 1);
        // Negative stride flips GL bottom-left origin to top-left for image viewers.
        if (!stbi_write_png(path.c_str(), w, h, 3, lastRow, -w * 3))
            Debug::Error("RenderSystem::DumpBuffers") << "stbi_write_png failed: " << path << "\n";
    }

    // ------------------------------------------------------------------
    //  ReinhardTonemap + gamma  (matches the Reinhard path in the shader)
    //  Input: linear HDR float, Output: sRGB uint8
    // ------------------------------------------------------------------
    static uint8_t TonemapChannel(float v, float exposure, float invGamma)
    {
        v *= exposure;
        v = v / (v + 1.0f);                          // Reinhard
        if (v < 0.0f) v = 0.0f;
        v = glm::pow(v, invGamma);                    // gamma correction
        if (v > 1.0f) v = 1.0f;
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
    }

    // ------------------------------------------------------------------
    //  DumpTexture2D_RGBA16F
    //  Reads an RGBA16F texture and writes a tonemapped PNG.
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
    //  Reads a DEPTH_COMPONENT32F texture and writes a greyscale PNG.
    //  Uses auto-ranging: scans the actual min/max depth values present
    //  in the buffer and stretches them to full [0, 255] contrast, so the
    //  result is always readable regardless of scene scale.
    //  Pixels at depth == 1.0 (cleared background) are remapped to black
    //  so empty sky doesn't dominate the range.
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

        // Find min/max among non-background pixels (depth < 1.0)
        float dMin = 1.0f, dMax = 0.0f;
        for (int i = 0; i < nPix; ++i) {
            float z = depth[i];
            if (z < 1.0f) {
                if (z < dMin) dMin = z;
                if (z > dMax) dMax = z;
            }
        }
        // If the whole buffer is background, fall back to full range
        const float range = (dMax > dMin) ? (dMax - dMin) : 1.0f;
        if (dMax <= dMin) { dMin = 0.0f; }

        std::vector<uint8_t> rgb(nPix * 3);
        for (int i = 0; i < nPix; ++i) {
            float z = depth[i];
            // Background pixels mapped to black so geometry stands out
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
    //  DumpCubeArrayFace
    //  Reads one face of one cubemap from a TEXTURE_CUBE_MAP_ARRAY and
    //  writes it as a greyscale PNG.
    //
    //  layer = lightIndex * 6 + faceIndex  (GL_TEXTURE_CUBE_MAP_ARRAY
    //  uses a flat array of 2D layers where each group of 6 is one cube).
    // ------------------------------------------------------------------
    static void DumpCubeArrayFace(GLuint tex,
        int res,
        int layer,          // absolute flat layer index
        const std::string& path)
    {
        const int nPix = res * res;
        std::vector<float> depth(nPix);

        // glGetTextureSubImage requires GL 4.5; use it when available so we
        // can pull a single face without a temporary FBO.
#if defined(GL_VERSION_4_5)
        glGetTextureSubImage(tex,
            /*level*/  0,
            /*xoff*/   0, /*yoff*/ 0, /*zoff*/ layer,
            /*w*/      res, /*h*/  res, /*d*/  1,
            GL_DEPTH_COMPONENT, GL_FLOAT,
            static_cast<GLsizei>(nPix * sizeof(float)),
            depth.data());
#else
    // Fallback: attach the desired layer to a temporary FBO and read back.
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, tex, 0, layer);
        glReadPixels(0, 0, res, res, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
#endif

        // Visualise raw [0, 1] depth stored in the shadow map (no linearisation
        // needed � it was already written as linear distance / farPlane).
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
    //  Reads an RGBA8 texture (e.g. the LDR tonemap output) and writes
    //  it directly as a PNG � no tonemapping needed, values are [0,255].
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

        // Pack into tightly-packed RGB (drop alpha)
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
    //  Reads the current back buffer (GL_BACK) with glReadPixels � this
    //  is the exact pixels that will be presented to the screen after
    //  FXAAPass, with no intermediate copies.
    //  Must be called AFTER FXAAPass() and BEFORE SwapBuffers().
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
    //  DumpTexture2D_NormalRoughness
    //  Reads the GBuffer RGBA16F normal+roughness attachment and writes:
    //    <base>_normal.png    — view-space normals remapped [-1,1]->[0,255]
    //                           (R=X, G=Y, B=Z — standard normal-map colours)
    //    <base>_roughness.png — roughness (w channel) as greyscale
    // ------------------------------------------------------------------
    static void DumpTexture2D_NormalRoughness(GLuint tex,
        int w, int h,
        const std::string& basePathNoExt)
    {
        const int nPix = w * h;
        std::vector<float> pixels(nPix * 4);

        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        std::vector<uint8_t> normalRGB(nPix * 3);
        std::vector<uint8_t> roughnessRGB(nPix * 3);

        for (int i = 0; i < nPix; ++i) {
            // Remap normal components [-1,1] -> [0,255]
            auto remapChan = [](float v) -> uint8_t {
                v = v * 0.5f + 0.5f;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                return static_cast<uint8_t>(v * 255.0f + 0.5f);
                };
            normalRGB[i * 3 + 0] = remapChan(pixels[i * 4 + 0]); // X -> R
            normalRGB[i * 3 + 1] = remapChan(pixels[i * 4 + 1]); // Y -> G
            normalRGB[i * 3 + 2] = remapChan(pixels[i * 4 + 2]); // Z -> B

            // Roughness: direct [0,1] -> [0,255] greyscale
            float r = pixels[i * 4 + 3];
            if (r < 0.0f) r = 0.0f;
            if (r > 1.0f) r = 1.0f;
            auto rb = static_cast<uint8_t>(r * 255.0f + 0.5f);
            roughnessRGB[i * 3 + 0] = rb;
            roughnessRGB[i * 3 + 1] = rb;
            roughnessRGB[i * 3 + 2] = rb;
        }

        WritePNG(basePathNoExt + "_normal.png", w, h, normalRGB);
        WritePNG(basePathNoExt + "_roughness.png", w, h, roughnessRGB);
    }

} // anonymous namespace

// =====================================================
//  DumpBuffers
// =====================================================
void RenderSystem::DumpBuffers() const
{
    // ---- Build timestamped subfolder  e.g. "Render/2025-08-01_14-30-05/" ----
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

    // ---- 1. HDR colour (post-shading, pre-tonemap) ----
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

    // ---- 3. GBuffer: view-space normals + roughness ----
    if (m_gbufferNormalTex) {
        // Writes gbuffer_normal.png (view normals as RGB) + gbuffer_roughness.png (greyscale)
        DumpTexture2D_NormalRoughness(m_gbufferNormalTex, m_screenW, m_screenH,
            dumpDir + "/gbuffer");
        Debug::Info("RenderSystem::DumpBuffers") << "Saved gbuffer_normal.png + gbuffer_roughness.png\n";
    }

    // ---- 4. Bloom threshold (full res, pre-blur) ----
    if (m_bloomThreshTex) {
        DumpTexture2D_RGBA16F(m_bloomThreshTex, m_screenW, m_screenH,
            exposure, gamma, path("bloom_thresh.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved bloom_thresh.png\n";
    }

    // ---- 5. Bloom result (half res, post-blur, what gets composited) ----
    if (m_bloomPingTex) {
        const int bW = std::max(1, m_screenW / 2);
        const int bH = std::max(1, m_screenH / 2);
        DumpTexture2D_RGBA16F(m_bloomPingTex, bW, bH,
            1.0f, 1.0f, path("bloom_result.png")); // raw, no extra tonemap
        Debug::Info("RenderSystem::DumpBuffers") << "Saved bloom_result.png\n";
    }

    // ---- 6. LDR colour (post-tonemap, pre-FXAA) ----
    if (m_ldrTex) {
        DumpTexture2D_RGBA8(m_ldrTex, m_screenW, m_screenH, path("ldr_color.png"));
        Debug::Info("RenderSystem::DumpBuffers") << "Saved ldr_color.png\n";
    }

    // ---- 7. Final output — what the screen actually shows (post-FXAA) ----
    DumpDefaultFramebuffer(m_screenW, m_screenH, path("final_output.png"));
    Debug::Info("RenderSystem::DumpBuffers") << "Saved final_output.png\n";

    // ---- 8. Shadow cubemap faces ----
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

    Debug::Info("RenderSystem::DumpBuffers")
        << "Buffer dump complete -> '" << dumpDir << "/'\n";
}