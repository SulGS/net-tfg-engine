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
            break;
        }
    }

    // Quality preset
    QualityPreset m_preset = QualityPreset::High;
    int           m_baseMip = 0;
    bool          m_useCompression = true;

    // Render loop
    int m_targetFPS = 144;

    // Window
    WindowMode m_windowMode = WindowMode::Windowed;
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
};