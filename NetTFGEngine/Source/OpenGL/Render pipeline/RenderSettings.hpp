#pragma once

#include <glm/glm.hpp>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <vector>
#include <utility>
#include <cstdio>

#include "Utils/UserDataPath.hpp"

enum class QualityPreset
{
    VeryLow,
    Low,
    Medium,
    High,
    Ultra
};

// How the final pass scales the render resolution image to the window when they differ.
// Nearest: nearest-neighbour (blocky, pixel-exact), up or down.
// Bilinear: plain bilinear sample (also what FSR1 falls back to when downscaling).
// FSR1: AMD FidelityFX Super Resolution 1 (EASU upscale + RCAS sharpen), only when the render resolution is lower.
// Explicit values: they are what render_settings.cfg stores ("upscaleMode"), so new modes go at the end.
enum class UpscaleMode
{
    Bilinear = 0,
    FSR1     = 1,
    Nearest  = 2,
};

// Windowed: normal decorated window. Borderless: undecorated window sized to
// cover the monitor (a.k.a. "borderless fullscreen"), keeps alt-tab fast.
// Fullscreen: exclusive fullscreen via glfwSetWindowMonitor.
enum class WindowMode
{
    Windowed,
    Borderless,
    Fullscreen
};

// Global singleton. [Init-time] settings must be set before RenderSystem::Init() (or re-Init() to apply); [Runtime] settings can change anytime.
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

    // QUALITY PRESET
    void          setPreset(QualityPreset preset) { m_preset = preset; applyPreset(); }
    QualityPreset getPreset()               const { return m_preset; }

    int  texBaseMip()     const { return m_baseMip; }
    bool texCompression() const { return m_useCompression; }

    // RUNTIME — RENDER LOOP: caps ClientWindow's render thread rate (see renderLoop()). Not part of applyPreset():
    // a quality tier shouldn't dictate frame rate, and resetToPreset() must not change it.
    void setTargetFPS(int v) { m_targetFPS = std::clamp(v, 1, 360); }
    int  getTargetFPS() const { return m_targetFPS; }

    // RUNTIME — WINDOW: just persisted state here; ClientWindow/OpenGLWindow
    // (which own the GLFW window) apply it. Deliberately not part of
    // applyPreset(), same reasoning as targetFPS above.
    void       setWindowMode(WindowMode v) { m_windowMode = v; }
    WindowMode getWindowMode() const { return m_windowMode; }

    // RUNTIME — WINDOW: window resolution, only used in Windowed mode (Borderless/Fullscreen are always the
    // monitor's native size). OpenGLWindow::setWindowedSize() applies it and clamps it to the monitor.
    void setWindowResolution(int w, int h) { m_windowWidth = std::max(320, w); m_windowHeight = std::max(240, h); }
    int  getWindowWidth()  const { return m_windowWidth; }
    int  getWindowHeight() const { return m_windowHeight; }

    // RUNTIME — RENDER RESOLUTION: height of the internal render targets; the width follows the window's aspect
    // ratio so pixels stay square, and the final pass scales the image to the window. 0 = native (same as the
    // window). Applied next frame by IECSGameRenderer (RenderSystem::Resize), no reinit needed.
    void setRenderHeight(int v) { m_renderHeight = (v <= 0) ? 0 : std::clamp(v, 144, 4320); }
    int  getRenderHeight() const { return m_renderHeight; }

    // Internal render size for an output (window framebuffer) of outW x outH.
    void computeRenderSize(int outW, int outH, int& renderW, int& renderH) const
    {
        if (m_renderHeight <= 0 || outW <= 0 || outH <= 0) { renderW = outW; renderH = outH; return; }
        renderH = m_renderHeight;
        renderW = std::max(1, static_cast<int>(static_cast<long long>(outW) * renderH / outH));
    }

    // RUNTIME — UPSCALING: filter that scales a lower render resolution up to the window (see UpscaleMode). Not part
    // of applyPreset(), like the render resolution it goes with. Sharpness is RCAS's: 0 = none .. 1 = maximum.
    void        setUpscaleMode(UpscaleMode v) { m_upscaleMode = v; }
    UpscaleMode getUpscaleMode() const { return m_upscaleMode; }

    void  setFSRSharpness(float v) { m_fsrSharpness = std::clamp(v, 0.0f, 1.0f); }
    float getFSRSharpness() const { return m_fsrSharpness; }

    // RUNTIME — WINDOW: same as above; OpenGLWindow applies it via
    // setVSync(). Off by default so the targetFPS pacer above is the only
    // frame cap unless the player opts back into vsync.
    void setVsyncEnabled(bool v) { m_vsyncEnabled = v; }
    bool getVsyncEnabled() const { return m_vsyncEnabled; }

    // RUNTIME — DEBUG: shows an FPS/network-latency overlay. Not part of
    // applyPreset(): purely a developer/player toggle, unrelated to quality.
    void setDebugModeEnabled(bool v) { m_debugModeEnabled = v; }
    bool getDebugModeEnabled() const { return m_debugModeEnabled; }

    // INIT-TIME SETTINGS
    void setMaxLights(int v) { m_maxLights = v; }
    int  getMaxLights()      const { return m_maxLights; }

    void setMaxShadowLights(int v) { m_maxShadowLights = v; }
    int  getMaxShadowLights()const { return m_maxShadowLights; }

    void setMsaaSamples(int v) { m_msaaSamples = std::max(1, v); }
    int  getMsaaSamples()    const { return m_msaaSamples; }

    void  setAnisotropy(float v) { m_anisotropy = v; }
    float getAnisotropy()    const { return m_anisotropy; }

    // RUNTIME — POINT LIGHT SHADOWS

    // Point light cubemap shadow resolution (width == height); call RenderSystem::ReInitShadows() after changing.
    void setShadowResolution(int v) { m_shadowRes = v; }
    int  getShadowResolution()const { return m_shadowRes; }

    void setPointShadowsEnabled(bool v) { m_shadowsEnabled = v; }
    bool getPointShadowsEnabled()  const { return m_shadowsEnabled; }

    void  setShadowNearPlane(float v) { m_shadowNearPlane = v; }
    float getShadowNearPlane()  const { return m_shadowNearPlane; }

    void  setShadowBiasFactor(float v) { m_shadowBiasFactor = v; }
    float getShadowBiasFactor()  const { return m_shadowBiasFactor; }

    void  setShadowBiasUnits(float v) { m_shadowBiasUnits = v; }
    float getShadowBiasUnits()   const { return m_shadowBiasUnits; }

    // RUNTIME — DIRECTIONAL LIGHT SHADOW: independent of point light shadows (getDirShadowsEnabled is a separate master toggle); resolution needs ReInitShadows() after changing, but extent/near/far apply next frame with no GPU recreation. Extent is the ortho frustum half-size; near/far are clip distances with the eye pulled back along -lightDir, centred on the world origin.

    void setDirShadowsEnabled(bool v) { m_dirShadowsEnabled = v; }
    bool getDirShadowsEnabled() const { return m_dirShadowsEnabled; }

    // Directional shadow map resolution (independent of point light res).
    // Call RenderSystem::ReInitShadows() after changing.
    void setDirShadowResolution(int v) { m_dirShadowRes = v; }
    int  getDirShadowResolution() const { return m_dirShadowRes; }

    void  setDirShadowExtent(float v) { m_dirShadowExtent = v; }
    float getDirShadowExtent()  const { return m_dirShadowExtent; }

    void  setDirShadowNear(float v) { m_dirShadowNear = v; }
    float getDirShadowNear()    const { return m_dirShadowNear; }

    void  setDirShadowFar(float v) { m_dirShadowFar = v; }
    float getDirShadowFar()     const { return m_dirShadowFar; }

    // RUNTIME — HDR / TONEMAPPING
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

    // RUNTIME — BLOOM
    void setBloomEnabled(bool v) { m_bloomEnabled = v; }
    bool getBloomEnabled()       const { return m_bloomEnabled; }

    void  setBloomThreshold(float v) { m_bloomThreshold = v; }
    float getBloomThreshold()    const { return m_bloomThreshold; }

    void  setBloomStrength(float v) { m_bloomStrength = v; }
    float getBloomStrength()     const { return m_bloomStrength; }

    void setBloomPasses(int v) { m_bloomPasses = v; }
    int  getBloomPasses()        const { return m_bloomPasses; }

    // RUNTIME — FXAA
    void setFXAAEnabled(bool v) { m_fxaaEnabled = v; }
    bool getFXAAEnabled()              const { return m_fxaaEnabled; }

    void  setFXAASubpix(float v) { m_fxaaSubpix = v; }
    float getFXAASubpix()              const { return m_fxaaSubpix; }

    void  setFXAAEdgeThreshold(float v) { m_fxaaEdgeThreshold = v; }
    float getFXAAEdgeThreshold()       const { return m_fxaaEdgeThreshold; }

    void  setFXAAEdgeThresholdMin(float v) { m_fxaaEdgeThresholdMin = v; }
    float getFXAAEdgeThresholdMin()    const { return m_fxaaEdgeThresholdMin; }

    // RUNTIME — SSAO: computed from the GBuffer right after GBufferPass and multiplied into the lighting like the
    // material AO map (emissive untouched). Radius is in world units; samples is clamped to the kernel size (64).
    void setSSAOEnabled(bool v) { m_ssaoEnabled = v; }
    bool getSSAOEnabled()        const { return m_ssaoEnabled; }

    void setSSAOSamples(int v) { m_ssaoSamples = std::clamp(v, 4, 64); }
    int  getSSAOSamples()        const { return m_ssaoSamples; }

    void  setSSAORadius(float v) { m_ssaoRadius = v; }
    float getSSAORadius()        const { return m_ssaoRadius; }

    void  setSSAOBias(float v) { m_ssaoBias = v; }
    float getSSAOBias()          const { return m_ssaoBias; }

    void  setSSAOIntensity(float v) { m_ssaoIntensity = v; }
    float getSSAOIntensity()     const { return m_ssaoIntensity; }

    // Resolution divisor of the SSAO targets relative to the render resolution: 1 = full, 2 = half, 4 = quarter.
    // Applied next frame (RenderSystem recreates the screen-space targets), no reinit needed.
    void setSSAOResolutionScale(int v) { m_ssaoResolutionScale = ClampScreenSpaceScale(v); }
    int  getSSAOResolutionScale() const { return m_ssaoResolutionScale; }

    // RUNTIME — SSR: view-space ray march against the GBuffer depth, added on top of the shaded HDR scene before the
    // particles. maxDistance/thickness are in world units; surfaces rougher than maxRoughness get no reflection.
    void setSSREnabled(bool v) { m_ssrEnabled = v; }
    bool getSSREnabled()         const { return m_ssrEnabled; }

    void setSSRSteps(int v) { m_ssrSteps = std::clamp(v, 8, 256); }
    int  getSSRSteps()           const { return m_ssrSteps; }

    // Resolution divisor of the SSR trace (and its scene copy), same meaning as the SSAO one.
    void setSSRResolutionScale(int v) { m_ssrResolutionScale = ClampScreenSpaceScale(v); }
    int  getSSRResolutionScale() const { return m_ssrResolutionScale; }

    void  setSSRMaxDistance(float v) { m_ssrMaxDistance = v; }
    float getSSRMaxDistance()    const { return m_ssrMaxDistance; }

    void  setSSRThickness(float v) { m_ssrThickness = v; }
    float getSSRThickness()      const { return m_ssrThickness; }

    void  setSSRMaxRoughness(float v) { m_ssrMaxRoughness = v; }
    float getSSRMaxRoughness()   const { return m_ssrMaxRoughness; }

    void  setSSRIntensity(float v) { m_ssrIntensity = v; }
    float getSSRIntensity()      const { return m_ssrIntensity; }

    // RUNTIME — MOTION BLUR: camera + per-object, reconstructed from a velocity buffer (McGuire-style tile/neighbour
    // max, so blur spills past silhouettes). Strength is the shutter time as a fraction of a 1/60 s frame (0.5 = the
    // classic 180° shutter at 60 FPS); the blur length is independent of the actual frame rate. Samples per pixel.
    void setMotionBlurEnabled(bool v) { m_motionBlurEnabled = v; }
    bool getMotionBlurEnabled()        const { return m_motionBlurEnabled; }

    void  setMotionBlurStrength(float v) { m_motionBlurStrength = std::max(0.0f, v); }
    float getMotionBlurStrength()      const { return m_motionBlurStrength; }

    void setMotionBlurSamples(int v) { m_motionBlurSamples = std::clamp(v, 4, 32); }
    int  getMotionBlurSamples()        const { return m_motionBlurSamples; }

    // PERSISTENCE: render_settings.cfg holds every field (normal load/save path); render_quality.cfg holds just the preset index as a fallback when the other is missing. Loading is automatic in the constructor, on first use of the singleton.

    // Reads render_settings.cfg. Returns false when it is missing, empty,
    // unreadable or malformed, in which case NOTHING is applied and the
    // caller keeps whatever it had.
    bool loadSettings()
    {
        std::ifstream f(settingsPath());
        if (!f.is_open())
            return false;

        // Phase 1 — read and validate the whole file before applying anything, so a file that turns to garbage halfway doesn't leave a mix of saved and preset values.
        std::vector<std::pair<std::string, double>> entries;

        std::string key;
        while (f >> key)
        {
            double value = 0.0;
            if (!(f >> value))
                return false;              // key with no value, or not a number
            entries.emplace_back(key, value);
        }

        if (!f.eof())                      // stopped for a reason other than EOF
            return false;
        if (entries.empty())
            return false;

        // Phase 2 — apply. The preset goes first because applyPreset()
        // rewrites every field; the remaining keys then refine it.
        for (const auto& e : entries)
        {
            if (e.first != "preset")
                continue;

            const int index = static_cast<int>(e.second);
            if (index < 0 || index > 4)
                return false;

            m_preset = static_cast<QualityPreset>(index);
            applyPreset();
            break;
        }

        int applied = 0;

        for (const auto& e : entries)
        {
            const std::string& k = e.first;
            const double  d = e.second;
            const bool    b = (d != 0.0);
            const int     i = static_cast<int>(d);
            const float   v = static_cast<float>(d);

            if (k == "preset") { /* ya aplicado arriba */ }
            else if (k == "targetFPS")            setTargetFPS(i);
            else if (k == "windowMode")
            {
                if (i >= 0 && i <= 2) setWindowMode(static_cast<WindowMode>(i));
            }
            else if (k == "windowWidth")          setWindowResolution(i, m_windowHeight);
            else if (k == "windowHeight")         setWindowResolution(m_windowWidth, i);
            else if (k == "renderHeight")         setRenderHeight(i);
            else if (k == "upscaleMode")
            {
                if (i >= 0 && i <= static_cast<int>(UpscaleMode::Nearest)) setUpscaleMode(static_cast<UpscaleMode>(i));
            }
            else if (k == "fsrSharpness")         setFSRSharpness(v);
            else if (k == "vsync")                setVsyncEnabled(b);
            else if (k == "debugMode")            setDebugModeEnabled(b);
            else if (k == "maxLights")            setMaxLights(i);
            else if (k == "maxShadowLights")      setMaxShadowLights(i);
            else if (k == "msaaSamples")          setMsaaSamples(i);
            else if (k == "anisotropy")           setAnisotropy(v);

            else if (k == "pointShadows")         setPointShadowsEnabled(b);
            else if (k == "shadowRes")            setShadowResolution(i);
            else if (k == "shadowNear")           setShadowNearPlane(v);
            else if (k == "shadowBiasFactor")     setShadowBiasFactor(v);
            else if (k == "shadowBiasUnits")      setShadowBiasUnits(v);

            else if (k == "dirShadows")           setDirShadowsEnabled(b);
            else if (k == "dirShadowRes")         setDirShadowResolution(i);
            else if (k == "dirShadowExtent")      setDirShadowExtent(v);
            else if (k == "dirShadowNear")        setDirShadowNear(v);
            else if (k == "dirShadowFar")         setDirShadowFar(v);

            else if (k == "exposure")             setExposure(v);
            else if (k == "filmic")               setFilmicEnabled(b);
            else if (k == "filmicShoulder")       setFilmicShoulder(v);
            else if (k == "filmicLinearStrength") setFilmicLinearStrength(v);
            else if (k == "filmicLinearAngle")    setFilmicLinearAngle(v);
            else if (k == "filmicToeStrength")    setFilmicToeStrength(v);
            else if (k == "filmicToeNumerator")   setFilmicToeNumerator(v);
            else if (k == "filmicToeDenominator") setFilmicToeDenominator(v);
            else if (k == "filmicLinearWhite")    setFilmicLinearWhite(v);
            else if (k == "gamma")                setGamma(v);

            else if (k == "bloom")                setBloomEnabled(b);
            else if (k == "bloomThreshold")       setBloomThreshold(v);
            else if (k == "bloomStrength")        setBloomStrength(v);
            else if (k == "bloomPasses")          setBloomPasses(i);

            else if (k == "fxaa")                 setFXAAEnabled(b);
            else if (k == "fxaaSubpix")           setFXAASubpix(v);
            else if (k == "fxaaEdgeThreshold")    setFXAAEdgeThreshold(v);
            else if (k == "fxaaEdgeThresholdMin") setFXAAEdgeThresholdMin(v);

            else if (k == "ssao")                 setSSAOEnabled(b);
            else if (k == "ssaoSamples")          setSSAOSamples(i);
            else if (k == "ssaoResolutionScale")  setSSAOResolutionScale(i);
            else if (k == "ssaoRadius")           setSSAORadius(v);
            else if (k == "ssaoBias")             setSSAOBias(v);
            else if (k == "ssaoIntensity")        setSSAOIntensity(v);

            else if (k == "ssr")                  setSSREnabled(b);
            else if (k == "ssrSteps")             setSSRSteps(i);
            else if (k == "ssrResolutionScale")   setSSRResolutionScale(i);
            else if (k == "ssrMaxDistance")       setSSRMaxDistance(v);
            else if (k == "ssrThickness")         setSSRThickness(v);
            else if (k == "ssrMaxRoughness")      setSSRMaxRoughness(v);
            else if (k == "ssrIntensity")         setSSRIntensity(v);

            else if (k == "motionBlur")           setMotionBlurEnabled(b);
            else if (k == "motionBlurStrength")   setMotionBlurStrength(v);
            else if (k == "motionBlurSamples")    setMotionBlurSamples(i);

            else continue;                 // unknown key: ignored on purpose,
            // so an older or newer file still
            // loads instead of being rejected

            ++applied;
        }

        // Nothing recognised at all: treat it as garbage rather than as a
        // successful load of zero settings.
        return applied > 0;
    }

    bool saveSettings() const
    {
        std::ofstream f(settingsPath());
        if (!f.is_open())
            return false;

        // preset first: on load it is applied before everything else, and the
        // remaining keys override it field by field.
        f << "preset " << static_cast<int>(m_preset) << "\n";

        f << "targetFPS " << m_targetFPS << "\n";
        f << "windowMode " << static_cast<int>(m_windowMode) << "\n";
        f << "windowWidth " << m_windowWidth << "\n";
        f << "windowHeight " << m_windowHeight << "\n";
        f << "renderHeight " << m_renderHeight << "\n";
        f << "upscaleMode " << static_cast<int>(m_upscaleMode) << "\n";
        f << "fsrSharpness " << m_fsrSharpness << "\n";
        f << "vsync " << (m_vsyncEnabled ? 1 : 0) << "\n";
        f << "debugMode " << (m_debugModeEnabled ? 1 : 0) << "\n";

        f << "maxLights " << m_maxLights << "\n";
        f << "maxShadowLights " << m_maxShadowLights << "\n";
        f << "msaaSamples " << m_msaaSamples << "\n";
        f << "anisotropy " << m_anisotropy << "\n";

        f << "pointShadows " << (m_shadowsEnabled ? 1 : 0) << "\n";
        f << "shadowRes " << m_shadowRes << "\n";
        f << "shadowNear " << m_shadowNearPlane << "\n";
        f << "shadowBiasFactor " << m_shadowBiasFactor << "\n";
        f << "shadowBiasUnits " << m_shadowBiasUnits << "\n";

        f << "dirShadows " << (m_dirShadowsEnabled ? 1 : 0) << "\n";
        f << "dirShadowRes " << m_dirShadowRes << "\n";
        f << "dirShadowExtent " << m_dirShadowExtent << "\n";
        f << "dirShadowNear " << m_dirShadowNear << "\n";
        f << "dirShadowFar " << m_dirShadowFar << "\n";

        f << "exposure " << m_exposure << "\n";
        f << "filmic " << (m_filmicEnabled ? 1 : 0) << "\n";
        f << "filmicShoulder " << m_filmicShoulder << "\n";
        f << "filmicLinearStrength " << m_filmicLinearStrength << "\n";
        f << "filmicLinearAngle " << m_filmicLinearAngle << "\n";
        f << "filmicToeStrength " << m_filmicToeStrength << "\n";
        f << "filmicToeNumerator " << m_filmicToeNumerator << "\n";
        f << "filmicToeDenominator " << m_filmicToeDenominator << "\n";
        f << "filmicLinearWhite " << m_filmicLinearWhite << "\n";
        f << "gamma " << m_gamma << "\n";

        f << "bloom " << (m_bloomEnabled ? 1 : 0) << "\n";
        f << "bloomThreshold " << m_bloomThreshold << "\n";
        f << "bloomStrength " << m_bloomStrength << "\n";
        f << "bloomPasses " << m_bloomPasses << "\n";

        f << "fxaa " << (m_fxaaEnabled ? 1 : 0) << "\n";
        f << "fxaaSubpix " << m_fxaaSubpix << "\n";
        f << "fxaaEdgeThreshold " << m_fxaaEdgeThreshold << "\n";
        f << "fxaaEdgeThresholdMin " << m_fxaaEdgeThresholdMin << "\n";

        f << "ssao " << (m_ssaoEnabled ? 1 : 0) << "\n";
        f << "ssaoSamples " << m_ssaoSamples << "\n";
        f << "ssaoResolutionScale " << m_ssaoResolutionScale << "\n";
        f << "ssaoRadius " << m_ssaoRadius << "\n";
        f << "ssaoBias " << m_ssaoBias << "\n";
        f << "ssaoIntensity " << m_ssaoIntensity << "\n";

        f << "ssr " << (m_ssrEnabled ? 1 : 0) << "\n";
        f << "ssrSteps " << m_ssrSteps << "\n";
        f << "ssrResolutionScale " << m_ssrResolutionScale << "\n";
        f << "ssrMaxDistance " << m_ssrMaxDistance << "\n";
        f << "ssrThickness " << m_ssrThickness << "\n";
        f << "ssrMaxRoughness " << m_ssrMaxRoughness << "\n";
        f << "ssrIntensity " << m_ssrIntensity << "\n";

        f << "motionBlur " << (m_motionBlurEnabled ? 1 : 0) << "\n";
        f << "motionBlurStrength " << m_motionBlurStrength << "\n";
        f << "motionBlurSamples " << m_motionBlurSamples << "\n";

        f.flush();
        return f.good();
    }

    void savePreset() const
    {
        std::ofstream f(cfgPath());
        if (f.is_open())
            f << static_cast<int>(m_preset) << "\n";
    }

    bool save() const
    {
        savePreset();
        return saveSettings();
    }

    // Back to the current preset's table values, throwing away any
    // fine-tuning, and removes render_settings.cfg so the next launch falls
    // back to render_quality.cfg instead of resurrecting what was discarded.
    void resetToPreset()
    {
        applyPreset();
        std::remove(settingsPath().c_str());
    }

private:
    // Resolved on every use (not cached) so they pick up the product name set by Debug::Initialize.
    static std::string cfgPath()      { return UserDataPath::File("render_quality.cfg"); }
    static std::string settingsPath() { return UserDataPath::File("render_settings.cfg"); }

    RenderSettings()
    {
        // The preset is the floor: it fills in every field.
        m_preset = loadPresetFromFile();
        applyPreset();

        // And render_settings.cfg refines it. If it is missing or corrupt,
        // loadSettings() applies nothing and we keep the preset values.
        loadSettings();
    }

    static QualityPreset loadPresetFromFile()
    {
        std::ifstream f(cfgPath());
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
			m_targetFPS = 30;
            m_baseMip = 3;
            m_useCompression = true;
            m_maxLights = 64;
            m_maxShadowLights = 0;
            m_msaaSamples = 1;
            m_anisotropy = 1.0f;
            m_shadowsEnabled = false;
            m_shadowRes = 128;
            m_shadowNearPlane = 0.5f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            m_dirShadowsEnabled = false;
            m_dirShadowRes = 512;
            m_dirShadowExtent = 30.0f;
            m_dirShadowNear = -50.0f;
            m_dirShadowFar = 50.0f;
            m_exposure = 1.0f;
            m_filmicEnabled = false;
            m_gamma = 2.2f;
            m_bloomEnabled = false;
            m_fxaaEnabled = false;
            m_ssaoEnabled = false;
            m_ssrEnabled = false;
            applyScreenSpaceDefaults(16, 4, 32, 4, false, 8);
            break;

        case QualityPreset::Low:
			m_targetFPS = 30;
            m_baseMip = 2;
            m_useCompression = true;
            m_maxLights = 128;
            m_maxShadowLights = 2;
            m_msaaSamples = 1;
            m_anisotropy = 2.0f;
            m_shadowsEnabled = false;
            m_shadowRes = 256;
            m_shadowNearPlane = 0.3f;
            m_shadowBiasFactor = 2.0f;
            m_shadowBiasUnits = 4.0f;
            m_dirShadowsEnabled = true;
            m_dirShadowRes = 1024;
            m_dirShadowExtent = 75.0f;
            m_dirShadowNear = -100.0f;
            m_dirShadowFar = 100.0f;
            m_exposure = 1.0f;
            m_filmicEnabled = false;
            m_gamma = 2.2f;
            m_bloomEnabled = false;
            m_fxaaEnabled = true;
            m_ssaoEnabled = false;
            m_ssrEnabled = false;
            applyScreenSpaceDefaults(16, 4, 32, 4, false, 8);
            break;

        case QualityPreset::Medium:
			m_targetFPS = 60;
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
            m_dirShadowRes = 2048;
            m_dirShadowExtent = 150.0f;
            m_dirShadowNear = -100.0f;
            m_dirShadowFar = 250.0f;
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
            m_ssaoEnabled = true;
            m_ssrEnabled = false;
            applyScreenSpaceDefaults(16, 2, 32, 4, false, 8);
            break;

        case QualityPreset::High:
			m_targetFPS = 144;
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
            m_dirShadowRes = 4096;
            m_dirShadowExtent = 300.0f;
            m_dirShadowNear = -100.0f;
            m_dirShadowFar = 500.0f;
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
            m_ssaoEnabled = true;
            m_ssrEnabled = true;
            applyScreenSpaceDefaults(32, 2, 48, 2, true, 12);
            break;

        case QualityPreset::Ultra:
			m_targetFPS = 240;
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
            m_dirShadowRes = 8192;
            // Ultra: larger frustum to cover expansive scenes at high res.
            m_dirShadowExtent = 600.0f;
            m_dirShadowNear = -150.0f;
            m_dirShadowFar = 800.0f;
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
            m_ssaoEnabled = true;
            m_ssrEnabled = true;
            applyScreenSpaceDefaults(64, 1, 96, 1, true, 16);
            break;
        }
    }

    // Screen-space resolution divisors: only 1, 2 or 4 (anything in between snaps down).
    static int ClampScreenSpaceScale(int v) { return v >= 4 ? 4 : (v >= 2 ? 2 : 1); }

    // SSAO/SSR/motion blur tuning shared by every preset; only the sample/step counts, the SSAO/SSR resolution
    // divisors (and whether motion blur is on) scale with quality.
    void applyScreenSpaceDefaults(int ssaoSamples, int ssaoScale, int ssrSteps, int ssrScale,
                                  bool motionBlur, int motionBlurSamples)
    {
        m_ssaoResolutionScale = ClampScreenSpaceScale(ssaoScale);
        m_ssrResolutionScale = ClampScreenSpaceScale(ssrScale);
        m_motionBlurEnabled = motionBlur;
        m_motionBlurSamples = motionBlurSamples;
        m_motionBlurStrength = 0.5f;
        m_ssaoSamples = ssaoSamples;
        m_ssaoRadius = 1.5f;
        m_ssaoBias = 0.05f;
        m_ssaoIntensity = 1.5f;
        m_ssrSteps = ssrSteps;
        m_ssrMaxDistance = 60.0f;
        m_ssrThickness = 1.5f;
        m_ssrMaxRoughness = 0.6f;
        m_ssrIntensity = 1.0f;
    }

    // Quality preset
    QualityPreset m_preset = QualityPreset::High;
    int           m_baseMip = 0;
    bool          m_useCompression = true;

    // Render loop
    int m_targetFPS = 144;

    // Window
    WindowMode m_windowMode = WindowMode::Windowed;
    int        m_windowWidth = 1280;
    int        m_windowHeight = 720;
    int        m_renderHeight = 0;   // 0 = native
    UpscaleMode m_upscaleMode = UpscaleMode::FSR1;
    float       m_fsrSharpness = 0.8f;
    bool       m_vsyncEnabled = false;

    // Debug overlay (FPS / network latency)
    bool m_debugModeEnabled = false;

    // Init-time
    int   m_maxLights = 512;
    int   m_maxShadowLights = 8;
    int   m_msaaSamples = 4;
    float m_anisotropy = 8.0f;

    // Runtime — point light shadows
    int   m_shadowRes = 1024;
    bool  m_shadowsEnabled = true;
    float m_shadowNearPlane = 0.1f;
    float m_shadowBiasFactor = 2.0f;
    float m_shadowBiasUnits = 4.0f;

    // Runtime — directional light shadow frustum
    bool  m_dirShadowsEnabled = true;  // independent toggle
    int   m_dirShadowRes = 2048;       // independent resolution
    float m_dirShadowExtent = 50.0f;
    float m_dirShadowNear = -100.0f;
    float m_dirShadowFar = 100.0f;

    // HDR / Tonemapping
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

    // Bloom
    bool  m_bloomEnabled = true;
    float m_bloomThreshold = 1.0f;
    float m_bloomStrength = 0.04f;
    int   m_bloomPasses = 4;

    // FXAA
    bool  m_fxaaEnabled = true;
    float m_fxaaSubpix = 0.75f;
    float m_fxaaEdgeThreshold = 0.125f;
    float m_fxaaEdgeThresholdMin = 0.0833f;

    // SSAO
    bool  m_ssaoEnabled = true;
    int   m_ssaoSamples = 32;
    int   m_ssaoResolutionScale = 2;
    float m_ssaoRadius = 1.5f;
    float m_ssaoBias = 0.05f;
    float m_ssaoIntensity = 1.5f;

    // SSR
    bool  m_ssrEnabled = true;
    int   m_ssrSteps = 48;
    int   m_ssrResolutionScale = 2;
    float m_ssrMaxDistance = 60.0f;
    float m_ssrThickness = 1.5f;
    float m_ssrMaxRoughness = 0.6f;
    float m_ssrIntensity = 1.0f;

    // Motion blur
    bool  m_motionBlurEnabled = true;
    float m_motionBlurStrength = 0.5f;
    int   m_motionBlurSamples = 12;
};
