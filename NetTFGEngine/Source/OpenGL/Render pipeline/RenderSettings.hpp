#pragma once

#include <glm/glm.hpp>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>

// -------------------------------------------------------
// QualityPreset
// -------------------------------------------------------
enum class QualityPreset
{
    VeryLow,
    Low,
    Medium,
    High,
    Ultra
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
//  [Runtime]      Can be changed at any time.
// -------------------------------------------------------
class RenderSettings
{
public:
    static RenderSettings& instance()
    {
        static RenderSettings inst;
        return inst;
    }

    RenderSettings(const RenderSettings&) = delete;
    RenderSettings& operator=(const RenderSettings&) = delete;
    RenderSettings(RenderSettings&&) = delete;
    RenderSettings& operator=(RenderSettings&&) = delete;

    // =======================================================
    //  QUALITY PRESET
    // =======================================================
    void          setPreset(QualityPreset preset) { m_preset = preset; applyPreset(); }
    QualityPreset getPreset()               const { return m_preset; }

    int  texBaseMip()     const { return m_baseMip; }
    bool texCompression() const { return m_useCompression; }

    // =======================================================
    //  INIT-TIME SETTINGS
    // =======================================================

    void setMaxLights(int v) { m_maxLights = v; }
    int  getMaxLights()      const { return m_maxLights; }

    void setMaxShadowLights(int v) { m_maxShadowLights = v; }
    int  getMaxShadowLights()const { return m_maxShadowLights; }

    void setMsaaSamples(int v) { m_msaaSamples = std::max(1, v); }
    int  getMsaaSamples()    const { return m_msaaSamples; }

    void  setAnisotropy(float v) { m_anisotropy = v; }
    float getAnisotropy()    const { return m_anisotropy; }

    // =======================================================
    //  RUNTIME — POINT LIGHT SHADOWS
    // =======================================================

    // Shadow map resolution (width == height).
    // Also used for the directional light shadow map.
    // Call RenderSystem::ReInitShadows() after changing.
    void setShadowResolution(int v) { m_shadowRes = v; }
    int  getShadowResolution()const { return m_shadowRes; }

    void setShadowsEnabled(bool v) { m_shadowsEnabled = v; }
    bool getShadowsEnabled()  const { return m_shadowsEnabled; }

    void  setShadowNearPlane(float v) { m_shadowNearPlane = v; }
    float getShadowNearPlane()  const { return m_shadowNearPlane; }

    void  setShadowBiasFactor(float v) { m_shadowBiasFactor = v; }
    float getShadowBiasFactor()  const { return m_shadowBiasFactor; }

    void  setShadowBiasUnits(float v) { m_shadowBiasUnits = v; }
    float getShadowBiasUnits()   const { return m_shadowBiasUnits; }

    // =======================================================
    //  RUNTIME — DIRECTIONAL LIGHT SHADOW
    //
    //  getDirShadowsEnabled() — master toggle for the directional
    //                           shadow map pass.  When false,
    //                           DirShadowPass is skipped entirely
    //                           and the shader receives no occlusion
    //                           from the directional light.
    //                           Independent of getShadowsEnabled()
    //                           so point light shadows can remain on
    //                           while directional shadows are off
    //                           (useful for low-end or VeryLow preset).
    //
    //  getDirShadowExtent()   — half-width and half-height of
    //                           the ortho frustum in world units.
    //                           Increase if shadows clip at scene edges.
    //
    //  getDirShadowNear() /
    //  getDirShadowFar()      — near and far clip distances along
    //                           the light direction.  The eye is pulled
    //                           back getDirShadowFar()*0.5 from the
    //                           origin along -lightDir, centring the
    //                           frustum on the world origin.
    //                           Default: near=-100, far=100.
    //
    //  Changing any of these takes effect on the next frame with
    //  no GPU resource recreation needed.
    // =======================================================

    // Master on/off for the directional shadow map pass.
    void setDirShadowsEnabled(bool v) { m_dirShadowsEnabled = v; }
    bool getDirShadowsEnabled() const { return m_dirShadowsEnabled; }

    void  setDirShadowExtent(float v) { m_dirShadowExtent = v; }
    float getDirShadowExtent()  const { return m_dirShadowExtent; }

    void  setDirShadowNear(float v) { m_dirShadowNear = v; }
    float getDirShadowNear()    const { return m_dirShadowNear; }

    void  setDirShadowFar(float v) { m_dirShadowFar = v; }
    float getDirShadowFar()     const { return m_dirShadowFar; }

    // =======================================================
    //  RUNTIME — HDR / TONEMAPPING
    // =======================================================

    void  setExposure(float v) { m_exposure = v; }
    float getExposure()         const { return m_exposure; }

    void setFilmicEnabled(bool v) { m_filmicEnabled = v; }
    bool getFilmicEnabled()     const { return m_filmicEnabled; }

    void  setFilmicShoulder(float v) { m_filmicShoulder = v; }
    float getFilmicShoulder()        const { return m_filmicShoulder; }

    void  setFilmicLinearStrength(float v) { m_filmicLinearStrength = v; }
    float getFilmicLinearStrength()  const { return m_filmicLinearStrength; }

    void  setFilmicLinearAngle(float v) { m_filmicLinearAngle = v; }
    float getFilmicLinearAngle()     const { return m_filmicLinearAngle; }

    void  setFilmicToeStrength(float v) { m_filmicToeStrength = v; }
    float getFilmicToeStrength()     const { return m_filmicToeStrength; }

    void  setFilmicToeNumerator(float v) { m_filmicToeNumerator = v; }
    float getFilmicToeNumerator()    const { return m_filmicToeNumerator; }

    void  setFilmicToeDenominator(float v) { m_filmicToeDenominator = v; }
    float getFilmicToeDenominator()  const { return m_filmicToeDenominator; }

    void  setFilmicLinearWhite(float v) { m_filmicLinearWhite = v; }
    float getFilmicLinearWhite()     const { return m_filmicLinearWhite; }

    void  setGamma(float v) { m_gamma = v; }
    float getGamma()  const { return m_gamma; }

    // =======================================================
    //  RUNTIME — BLOOM
    // =======================================================

    void setBloomEnabled(bool v) { m_bloomEnabled = v; }
    bool getBloomEnabled()       const { return m_bloomEnabled; }

    void  setBloomThreshold(float v) { m_bloomThreshold = v; }
    float getBloomThreshold()    const { return m_bloomThreshold; }

    void  setBloomStrength(float v) { m_bloomStrength = v; }
    float getBloomStrength()     const { return m_bloomStrength; }

    void setBloomPasses(int v) { m_bloomPasses = v; }
    int  getBloomPasses()        const { return m_bloomPasses; }

    // =======================================================
    //  RUNTIME — FXAA
    // =======================================================

    void setFXAAEnabled(bool v) { m_fxaaEnabled = v; }
    bool getFXAAEnabled()              const { return m_fxaaEnabled; }

    void  setFXAASubpix(float v) { m_fxaaSubpix = v; }
    float getFXAASubpix()              const { return m_fxaaSubpix; }

    void  setFXAAEdgeThreshold(float v) { m_fxaaEdgeThreshold = v; }
    float getFXAAEdgeThreshold()       const { return m_fxaaEdgeThreshold; }

    void  setFXAAEdgeThresholdMin(float v) { m_fxaaEdgeThresholdMin = v; }
    float getFXAAEdgeThresholdMin()    const { return m_fxaaEdgeThresholdMin; }

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

    static QualityPreset loadPresetFromFile()
    {
        std::ifstream f(CFG_PATH);
        if (!f.is_open())
            return QualityPreset::High;

        std::string line;
        std::getline(f, line);
        line.erase(std::remove_if(line.begin(), line.end(),
            [](unsigned char c) { return std::isspace(c); }), line.end());

        if (line.empty())
            return QualityPreset::High;

        if (line.size() == 1 && std::isdigit((unsigned char)line[0]))
        {
            int v = line[0] - '0';
            if (v >= 0 && v <= 4)
                return static_cast<QualityPreset>(v);
        }

        return QualityPreset::High;
    }

    void applyPreset()
    {
        switch (m_preset)
        {
        case QualityPreset::VeryLow:
            m_baseMip = 3;
            m_useCompression = true;
            m_maxLights = 64;
            m_maxShadowLights = 0;
            m_msaaSamples = 1;
            m_anisotropy = 1.0f;
            m_shadowsEnabled = false; // point light shadows off
            m_shadowRes = 128;
            m_shadowNearPlane = 0.5f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            m_dirShadowsEnabled = false; // directional shadows off
            m_dirShadowExtent = 30.0f;
            m_dirShadowNear = -50.0f;
            m_dirShadowFar = 50.0f;
            m_exposure = 1.0f;
            m_filmicEnabled = false;
            m_gamma = 2.2f;
            m_bloomEnabled = false;
            m_fxaaEnabled = false;
            break;

        case QualityPreset::Low:
            m_baseMip = 2;
            m_useCompression = true;
            m_maxLights = 128;
            m_maxShadowLights = 2;
            m_msaaSamples = 1;
            m_anisotropy = 2.0f;
            m_shadowsEnabled = false; // point light shadows off
            m_shadowRes = 256;
            m_shadowNearPlane = 0.3f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            m_dirShadowsEnabled = true;  // directional shadows on
            m_dirShadowExtent = 50.0f;
            m_dirShadowNear = -100.0f;
            m_dirShadowFar = 100.0f;
            m_baseMip = 1;
            m_useCompression = true;
            m_maxLights = 256;
            m_maxShadowLights = 4;
            m_msaaSamples = 2;
            m_anisotropy = 4.0f;
            m_shadowsEnabled = true;
            m_shadowRes = 512;
            m_shadowNearPlane = 0.2f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            m_dirShadowsEnabled = true;
            m_dirShadowExtent = 50.0f;
            m_dirShadowNear = -100.0f;
            m_dirShadowFar = 100.0f;
            m_exposure = 1.0f;
            m_filmicEnabled = true;
            m_filmicShoulder = 0.22f;
            m_filmicLinearStrength = 0.30f;
            m_filmicLinearAngle = 0.10f;
            m_filmicToeStrength = 0.20f;
            m_filmicToeNumerator = 0.01f;
            m_filmicToeDenominator = 0.30f;
            m_filmicLinearWhite = 11.2f;
            m_gamma = 2.2f;
            m_bloomEnabled = true;
            m_bloomThreshold = 0.65f;
            m_bloomStrength = 0.5f;
            m_bloomPasses = 3;
            m_fxaaEnabled = true;
            break;

        case QualityPreset::High:
            m_baseMip = 0;
            m_useCompression = true;
            m_maxLights = 512;
            m_maxShadowLights = 8;
            m_msaaSamples = 4;
            m_anisotropy = 8.0f;
            m_shadowsEnabled = true;
            m_shadowRes = 1024;
            m_shadowNearPlane = 0.1f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            m_dirShadowsEnabled = true;
            m_dirShadowExtent = 50.0f;
            m_dirShadowNear = -100.0f;
            m_dirShadowFar = 100.0f;
            m_exposure = 1.0f;
            m_filmicEnabled = true;
            m_filmicShoulder = 0.22f;
            m_filmicLinearStrength = 0.30f;
            m_filmicLinearAngle = 0.10f;
            m_filmicToeStrength = 0.20f;
            m_filmicToeNumerator = 0.01f;
            m_filmicToeDenominator = 0.30f;
            m_filmicLinearWhite = 11.2f;
            m_gamma = 2.2f;
            m_bloomEnabled = true;
            m_bloomThreshold = 0.65f;
            m_bloomStrength = 0.25f;
            m_bloomPasses = 5;
            m_fxaaEnabled = true;
            break;

        case QualityPreset::Ultra:
            m_baseMip = 0;
            m_useCompression = false;
            m_maxLights = 1024;
            m_maxShadowLights = 16;
            m_msaaSamples = 8;
            m_anisotropy = 16.0f;
            m_shadowsEnabled = true;
            m_shadowRes = 2048;
            m_shadowNearPlane = 0.05f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            m_dirShadowsEnabled = true;
            // Ultra: larger frustum to cover expansive scenes at high res.
            m_dirShadowExtent = 75.0f;
            m_dirShadowNear = -150.0f;
            m_dirShadowFar = 150.0f;
            m_exposure = 1.2f;
            m_filmicEnabled = true;
            m_filmicShoulder = 0.22f;
            m_filmicLinearStrength = 0.30f;
            m_filmicLinearAngle = 0.10f;
            m_filmicToeStrength = 0.20f;
            m_filmicToeNumerator = 0.01f;
            m_filmicToeDenominator = 0.30f;
            m_filmicLinearWhite = 11.2f;
            m_gamma = 2.2f;
            m_bloomEnabled = true;
            m_bloomThreshold = 0.65f;
            m_bloomStrength = 0.25f;
            m_bloomPasses = 8;
            m_fxaaEnabled = true;
            break;
        }
    }

    // ---- Quality preset ----
    QualityPreset m_preset = QualityPreset::High;
    int           m_baseMip = 0;
    bool          m_useCompression = true;

    // ---- Init-time ----
    int   m_maxLights = 512;
    int   m_maxShadowLights = 8;
    int   m_msaaSamples = 4;
    float m_anisotropy = 8.0f;

    // ---- Runtime — point light shadows ----
    int   m_shadowRes = 1024;
    bool  m_shadowsEnabled = true;
    float m_shadowNearPlane = 0.1f;
    float m_shadowBiasFactor = 2.0f;
    float m_shadowBiasUnits = 4.0f;

    // ---- Runtime — directional light shadow frustum ----
    bool  m_dirShadowsEnabled = true;  // independent toggle
    float m_dirShadowExtent = 50.0f;
    float m_dirShadowNear = -100.0f;
    float m_dirShadowFar = 100.0f;

    // ---- HDR / Tonemapping ----
    float m_exposure = 1.0f;
    bool  m_filmicEnabled = true;
    float m_filmicShoulder = 0.22f;
    float m_filmicLinearStrength = 0.30f;
    float m_filmicLinearAngle = 0.10f;
    float m_filmicToeStrength = 0.20f;
    float m_filmicToeNumerator = 0.01f;
    float m_filmicToeDenominator = 0.30f;
    float m_filmicLinearWhite = 11.2f;
    float m_gamma = 2.2f;

    // ---- Bloom ----
    bool  m_bloomEnabled = true;
    float m_bloomThreshold = 1.0f;
    float m_bloomStrength = 0.04f;
    int   m_bloomPasses = 4;

    // ---- FXAA ----
    bool  m_fxaaEnabled = true;
    float m_fxaaSubpix = 0.75f;
    float m_fxaaEdgeThreshold = 0.125f;
    float m_fxaaEdgeThresholdMin = 0.0833f;
};