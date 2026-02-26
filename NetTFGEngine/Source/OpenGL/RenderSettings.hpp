#pragma once

#include <glm/glm.hpp>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>

// -------------------------------------------------------
// QualityPreset
// Controls texture resolution and compression tier.
// -------------------------------------------------------
enum class QualityPreset
{
    VeryLow,  // 1/8 res  + BC7 compressed  (~0.4% memory)
    Low,      // 1/4 res  + BC7 compressed  (~1.5% memory)
    Medium,   // 1/2 res  + BC7 compressed  (~6%   memory)
    High,     // Full res + BC7 compressed  (~25%  memory)
    Ultra     // Full res + uncompressed    (100%  memory)
};

// -------------------------------------------------------
// RenderSettings — global singleton
//
// Two categories of settings:
//
//  [Init-time]    Must be set BEFORE RenderSystem::Init().
//                 Changing them afterwards has no effect
//                 unless you call RenderSystem::Init() again.
//
//  [Runtime]      Can be changed at any time. RenderSystem
//                 reads them every frame or every pass.
//
// Usage:
//   RenderSettings::instance().setPreset(QualityPreset::High);
//   RenderSettings::instance().setShadowResolution(1024);
//   RenderSystem rs; rs.Init(1920, 1080);
// -------------------------------------------------------
class RenderSettings
{
public:
    // ---- Singleton access ----
    static RenderSettings& instance()
    {
        static RenderSettings inst;
        return inst;
    }

    // Delete copy / move
    RenderSettings(const RenderSettings&) = delete;
    RenderSettings& operator=(const RenderSettings&) = delete;
    RenderSettings(RenderSettings&&) = delete;
    RenderSettings& operator=(RenderSettings&&) = delete;

    // =======================================================
    //  QUALITY PRESET
    //  Applies to texture loading — set before loading assets.
    // =======================================================
    void          setPreset(QualityPreset preset) { m_preset = preset; applyPreset(); }
    QualityPreset getPreset()               const { return m_preset; }

    // Derived texture settings consumed by the asset loader
    int  texBaseMip()     const { return m_baseMip; }
    bool texCompression() const { return m_useCompression; }

    // =======================================================
    //  INIT-TIME SETTINGS
    //  Read once inside RenderSystem::Init().
    // =======================================================

    // Maximum number of point lights streamed to the GPU per frame.
    // Determines the light SSBO size.
    void setMaxLights(int v) { m_maxLights = v; }
    int  getMaxLights()        const { return m_maxLights; }

    // Maximum number of lights visible in a single screen tile.
    // Determines the light-index SSBO size.
    void setMaxLightsPerTile(int v) { m_maxLightsPerTile = v; }
    int  getMaxLightsPerTile() const { return m_maxLightsPerTile; }

    // Screen tile size in pixels for the Forward+ light culling pass.
    // Must match local_size_x/y in light_cull.comp.
    void setTileSize(int v) { m_tileSize = v; }
    int  getTileSize()         const { return m_tileSize; }

    // How many shadow-casting point lights are supported simultaneously.
    // Each occupies 6 layers in the cubemap array texture.
    void setMaxShadowLights(int v) { m_maxShadowLights = v; }
    int  getMaxShadowLights()  const { return m_maxShadowLights; }

    // MSAA sample count — must be 1, 2, 4, or 8.
    // Requires recreating all screen-size FBOs (main color + depth).
    // Call RenderSystem::ReInitFramebuffers() after changing.
    void setMSAASamples(int v) { m_msaaSamples = v; }
    int  getMSAASamples()  const { return m_msaaSamples; }

    // Anisotropic filtering level applied to every texture at load time.
    // Common values: 1 (off), 2, 4, 8, 16.
    // The driver silently clamps to GL_MAX_TEXTURE_MAX_ANISOTROPY if
    // the requested value exceeds hardware support.
    // Requires reloading assets to take effect.
    void  setAnisotropy(float v) { m_anisotropy = v; }
    float getAnisotropy()  const { return m_anisotropy; }

    // =======================================================
    //  RUNTIME SETTINGS
    //  Read every frame / every pass by RenderSystem.
    // =======================================================

    // Shadow map resolution (width == height, applies to every cubemap face).
    // Changing this requires RenderSystem to re-create the cubemap array —
    // call RenderSystem::ReInitShadows() after changing.
    void setShadowResolution(int v) { m_shadowRes = v; }
    int  getShadowResolution()  const { return m_shadowRes; }

    // Enable / disable the entire shadow pass.
    void setShadowsEnabled(bool v) { m_shadowsEnabled = v; }
    bool getShadowsEnabled()    const { return m_shadowsEnabled; }

    // Near plane for the shadow projection frustum.
    // Larger values improve depth precision across the shadow range,
    // reducing acne and peter-panning on geometry far from the light.
    // Too large a value clips geometry very close to the light source.
    void  setShadowNearPlane(float v) { m_shadowNearPlane = v; }
    float getShadowNearPlane()  const { return m_shadowNearPlane; }

    // Depth bias applied during the shadow pass via glPolygonOffset.
    // factor: scales with the slope of the polygon.
    // units:  constant depth offset (in depth-buffer units).
    void  setShadowBiasFactor(float v) { m_shadowBiasFactor = v; }
    float getShadowBiasFactor()  const { return m_shadowBiasFactor; }

    void  setShadowBiasUnits(float v) { m_shadowBiasUnits = v; }
    float getShadowBiasUnits()   const { return m_shadowBiasUnits; }

    // Save the current preset index back to the cfg file.
    void savePreset() const
    {
        std::ofstream f(CFG_PATH);
        if (f.is_open())
            f << static_cast<int>(m_preset) << "\n";
    }

private:
    static constexpr const char* CFG_PATH = "render_quality.cfg";

    RenderSettings()
    {
        m_preset = loadPresetFromFile();
        applyPreset();
    }

    // Read render_quality.cfg and return the corresponding preset.
    // Falls back to High if the file is missing, empty, or contains
    // an out-of-range value.
    static QualityPreset loadPresetFromFile()
    {
        std::ifstream f(CFG_PATH);
        if (!f.is_open())
            return QualityPreset::High; // file not found — use default

        std::string line;
        std::getline(f, line);

        // Strip whitespace
        line.erase(std::remove_if(line.begin(), line.end(),
            [](unsigned char c) { return std::isspace(c); }), line.end());

        if (line.empty())
            return QualityPreset::High;

        // Must be a single digit 0–4
        if (line.size() == 1 && std::isdigit((unsigned char)line[0]))
        {
            int v = line[0] - '0';
            if (v >= 0 && v <= 4)
                return static_cast<QualityPreset>(v);
        }

        return QualityPreset::High; // malformed value — use default
    }

    void applyPreset()
    {
        switch (m_preset)
        {
        case QualityPreset::VeryLow:
            // Texture
            m_baseMip = 3;
            m_useCompression = true;
            // Init-time
            m_maxLights = 64;
            m_maxLightsPerTile = 32;
            m_tileSize = 32;
            m_maxShadowLights = 0;
            m_msaaSamples = 1;    // no MSAA
            m_anisotropy = 1.0f; // off
            // Runtime
            m_shadowsEnabled = false;
            m_shadowRes = 128;
            m_shadowNearPlane = 0.5f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            break;

        case QualityPreset::Low:
            m_baseMip = 2;
            m_useCompression = true;
            m_maxLights = 128;
            m_maxLightsPerTile = 64;
            m_tileSize = 32;
            m_maxShadowLights = 2;
            m_msaaSamples = 1;
            m_anisotropy = 2.0f;
            m_shadowsEnabled = true;
            m_shadowRes = 256;
            m_shadowNearPlane = 0.3f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            break;

        case QualityPreset::Medium:
            m_baseMip = 1;
            m_useCompression = true;
            m_maxLights = 256;
            m_maxLightsPerTile = 128;
            m_tileSize = 16;
            m_maxShadowLights = 4;
            m_msaaSamples = 2;
            m_anisotropy = 4.0f;
            m_shadowsEnabled = true;
            m_shadowRes = 512;
            m_shadowNearPlane = 0.2f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            break;

        case QualityPreset::High:
            m_baseMip = 0;
            m_useCompression = true;
            m_maxLights = 512;
            m_maxLightsPerTile = 256;
            m_tileSize = 16;
            m_maxShadowLights = 8;
            m_msaaSamples = 4;
            m_anisotropy = 8.0f;
            m_shadowsEnabled = true;
            m_shadowRes = 1024;
            m_shadowNearPlane = 0.1f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            break;

        case QualityPreset::Ultra:
            m_baseMip = 0;
            m_useCompression = false;
            m_maxLights = 1024;
            m_maxLightsPerTile = 256;
            m_tileSize = 16;
            m_maxShadowLights = 16;
            m_msaaSamples = 8;
            m_anisotropy = 16.0f;
            m_shadowsEnabled = true;
            m_shadowRes = 2048;
            m_shadowNearPlane = 0.05f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            break;
        }
    }

    // ---- Quality preset ----
    QualityPreset m_preset = QualityPreset::High;
    int           m_baseMip = 0;
    bool          m_useCompression = true;

    // ---- Init-time ----
    int   m_maxLights = 512;
    int   m_maxLightsPerTile = 256;
    int   m_tileSize = 16;
    int   m_maxShadowLights = 8;
    int   m_msaaSamples = 4;
    float m_anisotropy = 8.0f;

    // ---- Runtime ----
    int   m_shadowRes = 1024;
    bool  m_shadowsEnabled = true;
    float m_shadowNearPlane = 0.1f;
    float m_shadowBiasFactor = 2.0f;
    float m_shadowBiasUnits = 4.0f;
};