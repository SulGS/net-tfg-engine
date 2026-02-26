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
// RenderSettings � global singleton
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
    //  Applies to texture loading � set before loading assets.
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

    // MSAA sample count � must be 1, 2, 4, or 8.
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
    // Changing this requires RenderSystem to re-create the cubemap array �
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

    // =======================================================
    //  HDR & TONEMAPPING  [Runtime]
    //
    //  The shading pass renders into a floating-point (RGBA16F)
    //  HDR framebuffer.  A fullscreen post-process pass then
    //  applies exposure and tonemapping before writing to the
    //  default (LDR) framebuffer.
    //
    //  Operators available:
    //    Reinhard  — simple, never clips, slightly desaturates
    //    Filmic    — ACES-approximation (Hill/Narkowicz), more
    //                contrast and colour saturation; shoulder
    //                and toe controlled by the curve parameters
    //                below.  Recommended for most scenes.
    // =======================================================

    // Scene exposure multiplier applied before tonemapping.
    // 1.0 = neutral.  Values > 1 brighten, < 1 darken.
    void  setExposure(float v) { m_exposure = v; }
    float getExposure()  const { return m_exposure; }

    // Toggle between Reinhard (false) and Filmic/ACES (true).
    void setFilmicEnabled(bool v) { m_filmicEnabled = v; }
    bool getFilmicEnabled()  const { return m_filmicEnabled; }

    // ----- Filmic curve parameters (ACES approximation) -----
    // These shape the shoulder (bright roll-off) and toe (dark
    // lift) of the curve.  Sensible ranges are given below.
    // Changing them takes effect on the next frame with no
    // GPU resource recreation needed.

    // Shoulder strength — controls how aggressively bright values
    // are compressed.  Range [0.0, 1.0], default 0.22.
    void  setFilmicShoulder(float v) { m_filmicShoulder = v; }
    float getFilmicShoulder()  const { return m_filmicShoulder; }

    // Linear strength — the slope of the linear middle section.
    // Range [0.0, 1.0], default 0.30.
    void  setFilmicLinearStrength(float v) { m_filmicLinearStrength = v; }
    float getFilmicLinearStrength()  const { return m_filmicLinearStrength; }

    // Linear angle — blending factor between linear and
    // shoulder segments.  Range [0.0, 1.0], default 0.10.
    void  setFilmicLinearAngle(float v) { m_filmicLinearAngle = v; }
    float getFilmicLinearAngle()  const { return m_filmicLinearAngle; }

    // Toe strength — controls how dark values are lifted.
    // Range [0.0, 1.0], default 0.20.
    void  setFilmicToeStrength(float v) { m_filmicToeStrength = v; }
    float getFilmicToeStrength()  const { return m_filmicToeStrength; }

    // Toe numerator and denominator — fine-tune the toe shape.
    // Default: numerator 0.01, denominator 0.30.
    void  setFilmicToeNumerator(float v) { m_filmicToeNumerator = v; }
    float getFilmicToeNumerator()    const { return m_filmicToeNumerator; }

    void  setFilmicToeDenominator(float v) { m_filmicToeDenominator = v; }
    float getFilmicToeDenominator()  const { return m_filmicToeDenominator; }

    // Linear white point — input value that maps to pure white.
    // Increase to retain more midtone detail; default 11.2.
    void  setFilmicLinearWhite(float v) { m_filmicLinearWhite = v; }
    float getFilmicLinearWhite()  const { return m_filmicLinearWhite; }

    // Gamma correction applied after tonemapping (sRGB ≈ 2.2).
    // Set to 1.0 if your window surface is already sRGB.
    void  setGamma(float v) { m_gamma = v; }
    float getGamma()  const { return m_gamma; }

    // =======================================================
    //  SSAO — Screen-Space Ambient Occlusion  [Runtime]
    //
    //  Samples the depth buffer in a hemisphere around each
    //  fragment to estimate local occlusion.  The result is
    //  an R8 occlusion texture that multiplies the ambient
    //  term inside the tonemap composite pass.
    //
    //  All parameters are read every frame — no GPU resource
    //  recreation required when changing them.
    // =======================================================

    // Master toggle.
    void setSSAOEnabled(bool v) { m_ssaoEnabled = v; }
    bool getSSAOEnabled()  const { return m_ssaoEnabled; }

    // Number of hemisphere samples per pixel.
    // Higher = better quality, more ALU.  Must be <= 64.
    void setSSAOSamples(int v) { m_ssaoSamples = v; }
    int  getSSAOSamples()  const { return m_ssaoSamples; }

    // Hemisphere radius in view-space units.
    // Too large -> samples escape geometry, miss occlusion.
    // Too small -> captures only micro-detail.
    void  setSSAORadius(float v) { m_ssaoRadius = v; }
    float getSSAORadius()  const { return m_ssaoRadius; }

    // Occlusion bias prevents self-occlusion acne on flat
    // surfaces.  Default 0.025.
    void  setSSAOBias(float v) { m_ssaoBias = v; }
    float getSSAOBias()  const { return m_ssaoBias; }

    // Power curve on the raw occlusion factor.
    // Values > 1.0 darken occluded areas more aggressively.
    void  setSSAOPower(float v) { m_ssaoPower = v; }
    float getSSAOPower()  const { return m_ssaoPower; }

    // =======================================================
    //  Bloom  [Runtime]
    //
    //  Extracts pixels above a luminance threshold from the
    //  HDR buffer, blurs them with a dual-pass Kawase filter,
    //  and additively blends the result back before tonemap.
    //  All done in the HDR domain so the tonemap curve shapes
    //  the final glow naturally.
    // =======================================================

    // Master toggle.
    void setBloomEnabled(bool v) { m_bloomEnabled = v; }
    bool getBloomEnabled()  const { return m_bloomEnabled; }

    // Luminance threshold — fragments below this do not
    // contribute to the bloom buffer.  Default 1.0.
    void  setBloomThreshold(float v) { m_bloomThreshold = v; }
    float getBloomThreshold()  const { return m_bloomThreshold; }

    // Additive blend weight when compositing bloom onto
    // the HDR buffer before tonemapping.  Default 0.04.
    void  setBloomStrength(float v) { m_bloomStrength = v; }
    float getBloomStrength()  const { return m_bloomStrength; }

    // Number of Kawase downsample+upsample iterations.
    // Range [1, 8].  Each step roughly doubles the spread.
    void setBloomPasses(int v) { m_bloomPasses = v; }
    int  getBloomPasses()  const { return m_bloomPasses; }

    // =======================================================
    //  FXAA — Fast Approximate Anti-Aliasing  [Runtime]
    //
    //  Single fullscreen pass on the final LDR buffer (after
    //  tonemapping).  Detects edges via local luminance contrast
    //  and blends along them.  Complements MSAA by smoothing
    //  shader-aliasing on specular highlights that MSAA misses.
    //
    //  No GPU resource recreation needed when toggling or tuning.
    // =======================================================

    void setFXAAEnabled(bool v) { m_fxaaEnabled = v; }
    bool getFXAAEnabled()  const { return m_fxaaEnabled; }

    // Minimum edge threshold.  Edges below this local contrast
    // are skipped entirely, preserving flat gradients.
    // Sensible range [0.02, 0.1].  Default 0.0312 (NVIDIA Low).
    void  setFXAAEdgeThresholdMin(float v) { m_fxaaEdgeThresholdMin = v; }
    float getFXAAEdgeThresholdMin()  const { return m_fxaaEdgeThresholdMin; }

    // Maximum edge threshold controls overall sensitivity.
    // Lower values detect more edges but introduce more blurring.
    // Sensible range [0.063, 0.333].  Default 0.125 (NVIDIA High).
    void  setFXAAEdgeThreshold(float v) { m_fxaaEdgeThreshold = v; }
    float getFXAAEdgeThreshold()  const { return m_fxaaEdgeThreshold; }

    // Sub-pixel aliasing removal strength [0.0, 1.0].
    // Higher = more sub-pixel smoothing but slight softening.
    // Default 0.75.
    void  setFXAASubpixel(float v) { m_fxaaSubpixel = v; }
    float getFXAASubpixel()  const { return m_fxaaSubpixel; }

    // =======================================================
    //  SSR — Screen-Space Reflections  [Runtime]
    //
    //  Ray-marches the reflection vector in screen space using
    //  the HDR depth buffer written by ShadingPass.  Blends onto
    //  metallic surfaces (weighted by metallic * (1-roughness)).
    //  Runs before tonemapping so reflections are in HDR range.
    //
    //  No extra G-buffer needed — uses m_hdrDepthTex and
    //  m_hdrColorTex already present in the pipeline.
    // =======================================================

    void setSSREnabled(bool v) { m_ssrEnabled = v; }
    bool getSSREnabled()  const { return m_ssrEnabled; }

    // Maximum number of ray-march iterations per pixel.
    // More steps = longer reflection rays but higher ALU cost.
    void setSSRMaxSteps(int v) { m_ssrMaxSteps = v; }
    int  getSSRMaxSteps()  const { return m_ssrMaxSteps; }

    // Maximum reflection ray distance in view-space units.
    // Rays that travel further are faded out and discarded.
    void  setSSRMaxDistance(float v) { m_ssrMaxDistance = v; }
    float getSSRMaxDistance()  const { return m_ssrMaxDistance; }

    // Size of each ray-march step in view-space units.
    // Smaller = sharper intersections, more cost.
    void  setSSRStepSize(float v) { m_ssrStepSize = v; }
    float getSSRStepSize()  const { return m_ssrStepSize; }

    // Binary search refinement steps after a hit is found.
    // More steps = crisper reflection edges.  Range [0, 16].
    void setSSRBinarySteps(int v) { m_ssrBinarySteps = v; }
    int  getSSRBinarySteps()  const { return m_ssrBinarySteps; }

    // Surfaces with perceptualRoughness above this cutoff receive
    // no SSR (reflections would be too blurry to be meaningful).
    // Default 0.4.
    void  setSSRRoughnessCutoff(float v) { m_ssrRoughnessCutoff = v; }
    float getSSRRoughnessCutoff()  const { return m_ssrRoughnessCutoff; }

    // Screen-edge fade width as a fraction of screen size [0, 0.5].
    // Reflections fade to zero near the edge to avoid hard cutoffs.
    // Default 0.1.
    void  setSSRFadeDistance(float v) { m_ssrFadeDistance = v; }
    float getSSRFadeDistance()  const { return m_ssrFadeDistance; }

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
            return QualityPreset::High; // file not found � use default

        std::string line;
        std::getline(f, line);

        // Strip whitespace
        line.erase(std::remove_if(line.begin(), line.end(),
            [](unsigned char c) { return std::isspace(c); }), line.end());

        if (line.empty())
            return QualityPreset::High;

        // Must be a single digit 0�4
        if (line.size() == 1 && std::isdigit((unsigned char)line[0]))
        {
            int v = line[0] - '0';
            if (v >= 0 && v <= 4)
                return static_cast<QualityPreset>(v);
        }

        return QualityPreset::High; // malformed value � use default
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
            // HDR — Reinhard only; filmic too expensive at this tier
            m_exposure = 1.0f;
            m_filmicEnabled = false;
            m_gamma = 2.2f;
            // SSAO — disabled at VeryLow
            m_ssaoEnabled = false;
            m_ssaoSamples = 8;
            m_ssaoRadius = 0.5f;
            m_ssaoBias = 0.025f;
            m_ssaoPower = 2.0f;
            // Bloom — disabled at VeryLow
            m_bloomEnabled = false;
            m_bloomThreshold = 1.0f;
            m_bloomStrength = 0.03f;
            m_bloomPasses = 2;
            // FXAA — on at all presets (single-pass, nearly free)
            m_fxaaEnabled = true;
            m_fxaaEdgeThresholdMin = 0.0312f;
            m_fxaaEdgeThreshold = 0.25f;   // looser at low quality
            m_fxaaSubpixel = 0.75f;
            // SSR — disabled at VeryLow/Low (too expensive without depth prepass)
            m_ssrEnabled = false;
            m_ssrMaxSteps = 16;
            m_ssrMaxDistance = 50.0f;
            m_ssrStepSize = 0.2f;
            m_ssrBinarySteps = 4;
            m_ssrRoughnessCutoff = 0.4f;
            m_ssrFadeDistance = 0.1f;
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
            // HDR — Reinhard; filmic kept off to save fillrate
            m_exposure = 1.0f;
            m_filmicEnabled = false;
            m_gamma = 2.2f;
            // SSAO — minimal quality
            m_ssaoEnabled = true;
            m_ssaoSamples = 8;
            m_ssaoRadius = 0.5f;
            m_ssaoBias = 0.025f;
            m_ssaoPower = 2.0f;
            // Bloom — minimal passes
            m_bloomEnabled = true;
            m_bloomThreshold = 1.0f;
            m_bloomStrength = 0.03f;
            m_bloomPasses = 3;
            // FXAA
            m_fxaaEnabled = true;
            m_fxaaEdgeThresholdMin = 0.0312f;
            m_fxaaEdgeThreshold = 0.25f;
            m_fxaaSubpixel = 0.75f;
            // SSR — disabled at Low
            m_ssrEnabled = false;
            m_ssrMaxSteps = 16;
            m_ssrMaxDistance = 50.0f;
            m_ssrStepSize = 0.2f;
            m_ssrBinarySteps = 4;
            m_ssrRoughnessCutoff = 0.4f;
            m_ssrFadeDistance = 0.1f;
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
            // HDR — filmic enabled with conservative exposure
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
            // SSAO — medium quality
            m_ssaoEnabled = true;
            m_ssaoSamples = 16;
            m_ssaoRadius = 0.5f;
            m_ssaoBias = 0.025f;
            m_ssaoPower = 2.0f;
            // Bloom — moderate passes
            m_bloomEnabled = true;
            m_bloomThreshold = 1.0f;
            m_bloomStrength = 0.04f;
            m_bloomPasses = 5;
            // FXAA — tighter thresholds at Medium+
            m_fxaaEnabled = true;
            m_fxaaEdgeThresholdMin = 0.0312f;
            m_fxaaEdgeThreshold = 0.125f;
            m_fxaaSubpixel = 0.75f;
            // SSR — enabled from Medium up, conservative quality
            m_ssrEnabled = true;
            m_ssrMaxSteps = 32;
            m_ssrMaxDistance = 50.0f;
            m_ssrStepSize = 0.2f;
            m_ssrBinarySteps = 4;
            m_ssrRoughnessCutoff = 0.4f;
            m_ssrFadeDistance = 0.1f;
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
            // HDR — filmic with standard cinematic curve
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
            // SSAO — high quality
            m_ssaoEnabled = true;
            m_ssaoSamples = 32;
            m_ssaoRadius = 0.5f;
            m_ssaoBias = 0.025f;
            m_ssaoPower = 2.0f;
            // Bloom — full passes
            m_bloomEnabled = true;
            m_bloomThreshold = 1.0f;
            m_bloomStrength = 0.04f;
            m_bloomPasses = 6;
            // FXAA
            m_fxaaEnabled = true;
            m_fxaaEdgeThresholdMin = 0.0312f;
            m_fxaaEdgeThreshold = 0.125f;
            m_fxaaSubpixel = 0.75f;
            // SSR — high quality
            m_ssrEnabled = true;
            m_ssrMaxSteps = 64;
            m_ssrMaxDistance = 100.0f;
            m_ssrStepSize = 0.15f;
            m_ssrBinarySteps = 8;
            m_ssrRoughnessCutoff = 0.4f;
            m_ssrFadeDistance = 0.1f;
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
            // HDR — filmic with boosted exposure for HDR highlights
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
            // SSAO — maximum quality
            m_ssaoEnabled = true;
            m_ssaoSamples = 64;
            m_ssaoRadius = 0.5f;
            m_ssaoBias = 0.025f;
            m_ssaoPower = 2.0f;
            // Bloom — maximum passes, slightly more strength
            m_bloomEnabled = true;
            m_bloomThreshold = 0.9f;
            m_bloomStrength = 0.05f;
            m_bloomPasses = 8;
            // FXAA — highest quality subpixel at Ultra
            m_fxaaEnabled = true;
            m_fxaaEdgeThresholdMin = 0.0312f;
            m_fxaaEdgeThreshold = 0.063f;
            m_fxaaSubpixel = 1.0f;
            // SSR — maximum quality
            m_ssrEnabled = true;
            m_ssrMaxSteps = 128;
            m_ssrMaxDistance = 150.0f;
            m_ssrStepSize = 0.1f;
            m_ssrBinarySteps = 16;
            m_ssrRoughnessCutoff = 0.4f;
            m_ssrFadeDistance = 0.1f;
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

    // ---- SSAO ----
    bool  m_ssaoEnabled = true;
    int   m_ssaoSamples = 32;
    float m_ssaoRadius = 0.5f;
    float m_ssaoBias = 0.025f;
    float m_ssaoPower = 2.0f;

    // ---- Bloom ----
    bool  m_bloomEnabled = true;
    float m_bloomThreshold = 1.0f;
    float m_bloomStrength = 0.04f;
    int   m_bloomPasses = 5;

    // ---- FXAA ----
    bool  m_fxaaEnabled = true;
    float m_fxaaEdgeThresholdMin = 0.0312f;
    float m_fxaaEdgeThreshold = 0.125f;
    float m_fxaaSubpixel = 0.75f;

    // ---- SSR ----
    bool  m_ssrEnabled = true;
    int   m_ssrMaxSteps = 64;
    float m_ssrMaxDistance = 100.0f;
    float m_ssrStepSize = 0.15f;
    int   m_ssrBinarySteps = 8;
    float m_ssrRoughnessCutoff = 0.4f;
    float m_ssrFadeDistance = 0.1f;
};