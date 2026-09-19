#ifndef SETTINGS_SCENE_HPP
#define SETTINGS_SCENE_HPP

// Escena de ajustes graficos (IECSGameLogic + IECSGameRenderer propios, como StartScreenGame).
// RenderSettings es estado local de la maquina: no entra en el GameStateBlob ni viaja por red.
// Los widgets son una vista de RenderSettings (SettingsPanelSystem los resincroniza cada frame), nunca una copia.

#include "OpenGL/OpenGLIncludes.hpp"
#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "ecs/UI/UIButton.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UISlider.hpp"
#include "ecs/UI/UIDropdown.hpp"
#include "ecs/ecs_gamelogic.hpp"
#include "OpenGL/IECSGameRenderer.hpp"
#include <openssl/evp.h>

#include "OpenGL/Render pipeline/RenderSettings.hpp"
#include "NetTFG_Engine.hpp"

#include <string>
#include <vector>
#include <functional>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <memory>
#include <cstring>
#include <stdexcept>

// Navegacion: esta escena no sabe que existe el menu, solo guarda de donde vino y vuelve alli.

// Id con el que registras esta escena en main().
inline constexpr int SETTINGS_SCENE_ID = 2;

inline int g_settingsReturnScene = 0;

// Activacion y baja se difieren con Request*Client (aplicadas entre ticks) para evitar un
// use-after-free si el onClick, corriendo en el hilo de render, activara/desactivara sincronamente.
inline void OpenSettingsFrom(int callerScene)
{
    g_settingsReturnScene = callerScene;

    auto& engine = NetTFG_Engine::Get();
    engine.RequestActivateClient(SETTINGS_SCENE_ID);
    engine.RequestDeactivateClient(callerScene);
}

inline void CloseSettings()
{
    auto& engine = NetTFG_Engine::Get();
    engine.RequestActivateClient(g_settingsReturnScene);
    engine.RequestDeactivateClient(SETTINGS_SCENE_ID);
}


// Helpers
namespace SettingsUI
{
    inline std::string FmtBool(bool v) { return v ? "Activado" : "Desactivado"; }

    // Nombres de nivel genericos: los usa tanto el preset como los ajustes
    // sueltos que se exponen como nivel (calidad de sombras, cantidad de
    // luces), para que un jugador casual elija "Alto" en vez de un numero
    // crudo que no significa nada para el.
    inline std::vector<std::string> TierNames()
    {
        return { "Muy bajo", "Bajo", "Medio", "Alto", "Ultra" };
    }

    inline std::vector<std::string> PresetNames()
    {
        // El orden debe coincidir con enum class QualityPreset.
        return TierNames();
    }

    // Nombre del preset. Sale de PresetNames() a proposito: el dropdown y el
    // mensaje de estado tienen que decir lo mismo, y con dos listas separadas
    // se acaban desincronizando.
    inline std::string FmtPreset(QualityPreset preset)
    {
        const std::vector<std::string> names = PresetNames();
        const int index = static_cast<int>(preset);
        if (index < 0 || index >= static_cast<int>(names.size()))
            return "?";
        return names[index];
    }

    // Indice de la opcion mas cercana. Necesario porque los presets escriben
    // valores que pueden no estar en la lista del dropdown.
    inline int IndexOfNearest(const std::vector<int>& opts, int value)
    {
        int best = 0;
        int bestDiff = INT_MAX;
        for (size_t i = 0; i < opts.size(); ++i)
        {
            int d = std::abs(opts[i] - value);
            if (d < bestDiff) { bestDiff = d; best = static_cast<int>(i); }
        }
        return best;
    }

    inline std::vector<std::string> IntOptionNames(const std::vector<int>& opts,
        const std::string& suffix)
    {
        std::vector<std::string> names;
        names.reserve(opts.size());
        for (int v : opts) names.push_back(std::to_string(v) + suffix);
        return names;
    }
}

// Componentes

// Estado del panel. Se crea una unica instancia.
class SettingsPanelData : public IComponent
{
public:
    int tab = 0;

    // Capa a la que se sube temporalmente un dropdown abierto para que su
    // popup no quede tapado por las filas de debajo.
    int popupLayer = 0;

    // Recreaciones pendientes. Las levanta quien cambia el ajuste y las
    // atiende el sistema en el frame siguiente, no el onClick.
    bool pendingShadowReInit = false;
    bool pendingRenderReInit = false;

    // Mensaje temporal de confirmacion.
    std::string statusMessage;
    float statusTimer = 0.0f;

    void SetStatus(const std::string& message, float seconds = 2.5f)
    {
        statusMessage = message;
        statusTimer = seconds;
    }
};

// Va en cada entidad del panel: controla visibilidad y sincronizacion.
class SettingsWidget : public IComponent
{
public:
    enum class Vis
    {
        Tab,      // visible solo si su pestana es la activa
        Always    // cabecera, pestanas y pie
    };

    Vis vis = Vis::Tab;
    int tab = 0;

    // Capa normal de la entidad, para poder restaurarla tras subir un popup.
    int normalLayer = 0;

    // Callbacks de lectura. Solo se rellena el que corresponda al tipo de
    // widget de la entidad; el sistema los usa para reescribir la UI desde
    // RenderSettings en cada frame.
    std::function<std::string()> readText;    // UIText / UIButton
    std::function<float()>       readSlider;  // UISlider
    std::function<int()>         readChoice;  // UIDropdown (indice)
};

// Sistema del renderer de ajustes: visibilidad por pestana, sincroniza widgets desde
// RenderSettings, y deja un solo dropdown abierto a la vez (subido de capa).
class SettingsPanelSystem : public ISystem
{
public:
    void Update(EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime) override
    {
        SettingsPanelData* data = nullptr;
        auto dataQuery = entityManager.CreateQuery<SettingsPanelData>();
        for (auto [entity, d] : dataQuery) data = d;
        if (!data) return;

        // 0. Recreaciones diferidas y temporizador del mensaje (el generico solo se pone si no hay uno mejor ya puesto).
        if (data->pendingRenderReInit)
        {
            data->pendingRenderReInit = false;
            data->pendingShadowReInit = false;
			requestRenderReinit = true;
            if (data->statusMessage.empty()) data->SetStatus("Renderizado reinicializado");
        }
        else if (data->pendingShadowReInit)
        {
            data->pendingShadowReInit = false;
            requestRenderReinit = true;
            if (data->statusMessage.empty()) data->SetStatus("Sombras recreadas");
        }

        if (data->statusTimer > 0.0f)
        {
            data->statusTimer -= deltaTime;
            if (data->statusTimer <= 0.0f)
            {
                data->statusTimer = 0.0f;
                data->statusMessage.clear();
            }
        }

        auto visible = [data](SettingsWidget* w) -> bool
            {
                return w->vis == SettingsWidget::Vis::Always || w->tab == data->tab;
            };

        // 1. Visibilidad
        auto elementQuery = entityManager.CreateQuery<UIElement, SettingsWidget>();
        for (auto [entity, element, widget] : elementQuery)
        {
            element->isVisible = visible(widget);
            element->layer = widget->normalLayer;   // el paso 3 puede subirla
        }

        // 2a. Botones (toggles y navegacion)
        // Un boton oculto pero interactuable seguiria capturando clics.
        auto buttonQuery = entityManager.CreateQuery<UIButton, SettingsWidget>();
        for (auto [entity, button, widget] : buttonQuery)
        {
            button->isInteractable = visible(widget);
            if (widget->readText) button->text = widget->readText();
        }

        // 2b. Textos estaticos y de solo lectura
        auto textQuery = entityManager.CreateQuery<UIText, SettingsWidget>();
        for (auto [entity, text, widget] : textQuery)
        {
            if (widget->readText) text->text = widget->readText();
        }

        // 2c. Sliders
        auto sliderQuery = entityManager.CreateQuery<UISlider, SettingsWidget>();
        for (auto [entity, slider, widget] : sliderQuery)
        {
            slider->isInteractable = visible(widget);

            // Durante el arrastre manda el raton, no RenderSettings.
            if (widget->readSlider && slider->state != SliderState::DRAGGING)
                slider->SetValue(widget->readSlider(), false);
        }

        // 3. Dropdowns
        UIDropdown* openThisFrame = nullptr;

        auto dropdownQuery = entityManager.CreateQuery<UIElement, UIDropdown, SettingsWidget>();
        for (auto [entity, element, dropdown, widget] : dropdownQuery)
        {
            const bool vis = visible(widget);
            dropdown->isInteractable = vis;

            // Al cambiar de pestana el popup no debe quedarse flotando.
            if (!vis && dropdown->isOpen) dropdown->Close();

            if (dropdown->isOpen)
            {
                // Solo uno abierto: si hay varios, gana el que no era el
                // registrado como abierto (es decir, el que se acaba de abrir).
                if (openThisFrame)
                {
                    if (openThisFrame == m_openDropdown) { openThisFrame->Close(); openThisFrame = dropdown; }
                    else { dropdown->Close(); }
                }
                else
                {
                    openThisFrame = dropdown;
                }
            }

            // Sincronizar solo si esta cerrado: reescribir el indice mientras
            // esta abierto se comeria el resaltado del usuario.
            if (widget->readChoice && !dropdown->isOpen)
                dropdown->SelectIndex(widget->readChoice(), false);

            if (dropdown->isOpen) element->layer = data->popupLayer;
        }

        m_openDropdown = openThisFrame;
    }

private:
    UIDropdown* m_openDropdown = nullptr;
};

// Constructor del panel
class SettingsPanel
{
public:
    // Layout (coordenadas relativas al centro de pantalla; +y baja)
    static constexpr float PANEL_W = 940.0f;
    static constexpr float PANEL_H = 680.0f;

    static constexpr float TITLE_Y = -300.0f;
    static constexpr float TABS_Y = -252.0f;
    static constexpr float HINT_Y = -208.0f;
    static constexpr float FIRST_ROW_Y = -160.0f;
    static constexpr float ROW_H = 38.0f;
    static constexpr float FOOTER_Y = 280.0f;

    static constexpr float LABEL_X = -230.0f;
    static constexpr float LABEL_W = 380.0f;
    static constexpr float ROW_TEXT_H = 26.0f;

    static constexpr float SLIDER_X = 170.0f;
    static constexpr float SLIDER_W = 340.0f;
    static constexpr float SLIDER_H = 26.0f;

    static constexpr float DROPDOWN_X = 120.0f;
    static constexpr float DROPDOWN_W = 240.0f;
    static constexpr float DROPDOWN_H = 30.0f;

    static constexpr float TOGGLE_X = 120.0f;
    static constexpr float TOGGLE_W = 240.0f;
    static constexpr float TOGGLE_H = 30.0f;

    enum Tab
    {
        TAB_CALIDAD = 0,
        TAB_SONIDO,
        TAB_SOMBRAS,
        TAB_IMAGEN,
        TAB_EFECTOS,
        TAB_AVANZADO,
        TAB_COUNT
    };

    static void Register(EntityManager& em)
    {
        em.RegisterComponentType<SettingsPanelData>();
        em.RegisterComponentType<SettingsWidget>();
    }

    // Construye el panel en el mundo del renderer de la escena de ajustes.
    static SettingsPanelData* Build(EntityManager& em,
        int baseLayer = 40,
        bool withBackground = false)
    {
        Entity dataEntity = em.CreateEntity();
        SettingsPanelData* data =
            em.AddComponent<SettingsPanelData>(dataEntity, SettingsPanelData{});
        data->popupLayer = baseLayer + 30;

        // Fondo opcional
        // Necesita una textura en disco. Si no tienes una, dejalo desactivado
        // y el panel se dibuja sobre el menu.
        if (withBackground)
        {
            Entity bg = em.CreateEntity();
            UIElement* el = em.AddComponent<UIElement>(bg, UIElement{});
            el->anchor = UIAnchor::CENTER;
            el->position = glm::vec2(0.0f, 0.0f);
            el->size = glm::vec2(PANEL_W, PANEL_H);
            el->pivot = glm::vec2(0.5f, 0.5f);
            el->isVisible = false;
            el->layer = baseLayer;

            UIImage* img = em.AddComponent<UIImage>(bg, UIImage{});
            img->texturePath = "ui_panel.png";

            Tag(em, bg, SettingsWidget::Vis::Always, 0, baseLayer);
        }

        BuildChrome(em, data, baseLayer);
        BuildQualityTab(em, data, baseLayer);
        BuildSoundTab(em, data, baseLayer);
        BuildShadowsTab(em, data, baseLayer);
        BuildImageTab(em, data, baseLayer);
        BuildEffectsTab(em, data, baseLayer);
        BuildAdvancedTab(em, data, baseLayer);
        BuildFooter(em, data, baseLayer);

        return data;
    }

private:
    // Cabecera, pestanas y pie

    static void BuildChrome(EntityManager& em, SettingsPanelData* data, int baseLayer)
    {
        // El boton que lleva a esta escena vive en el menu principal
        // (ver menu.hpp); aqui solo esta el de vuelta, en el pie.
        MakeText(em, glm::vec2(0.0f, TITLE_Y), glm::vec2(420.0f, 40.0f),
            baseLayer + 1, "Ajustes", 24.0f,
            SettingsWidget::Vis::Always, 0);

        static const char* tabNames[TAB_COUNT] =
        { "Calidad", "Sonido", "Sombras", "Imagen", "Efectos", "Avanzado" };

        // Sized to fit whatever TAB_COUNT is, with a small margin either
        // side of the panel, instead of a fixed width that only fit 5 tabs.
        const float tabStep = std::min(174.0f, (PANEL_W - 40.0f) / static_cast<float>(TAB_COUNT));
        const float tabW = tabStep - 10.0f;
        const float tabStart = -((TAB_COUNT - 1) * tabStep) * 0.5f;

        for (int i = 0; i < TAB_COUNT; ++i)
        {
            const float x = tabStart + tabStep * i;
            const char* name = tabNames[i];

            MakeButton(em, glm::vec2(x, TABS_Y), glm::vec2(tabW, 32.0f),
                baseLayer + 2, name,
                [data, i]() { data->tab = i; },
                SettingsWidget::Vis::Always, 0,
                // La pestana activa se marca con corchetes.
                [data, i, name]() -> std::string
                {
                    return (data->tab == i)
                        ? std::string("[ ") + name + " ]"
                        : std::string(name);
                });
        }

        // Ya no hay aviso de "reinicia el juego": los cuatro ajustes
        // init-time se aplican llamando otra vez a RenderSystem::Init().
    }

    static void BuildFooter(EntityManager& em, SettingsPanelData* data, int baseLayer)
    {
        // Confirmacion de lo que acaba de pasar, encima de los botones que la
        // provocan. Guardar escribe un fichero y restaurar solo mueve unos
        // sliders: sin esto, las dos acciones parecen no hacer nada.
        MakeText(em, glm::vec2(0.0f, FOOTER_Y - 42.0f), glm::vec2(760.0f, 24.0f),
            baseLayer + 1, "", 15.0f,
            SettingsWidget::Vis::Always, 0,
            [data]() { return data->statusMessage; },
            glm::vec4(0.55f, 1.0f, 0.70f, 1.0f));

        MakeButton(em, glm::vec2(-270.0f, FOOTER_Y), glm::vec2(250.0f, 36.0f),
            baseLayer + 2, "Restaurar preset",
            [data]()
            {
                RenderSettings& rs = RenderSettings::instance();
                // Vuelve a los valores de tabla del preset y borra
                // render_settings.cfg, para que el reset no resucite
                // en el siguiente arranque.
                rs.resetToPreset();
                data->pendingRenderReInit = true;
                data->SetStatus("Preset restaurado: " +
                    SettingsUI::FmtPreset(rs.getPreset()));
            },
            SettingsWidget::Vis::Always, 0);

        MakeButton(em, glm::vec2(0.0f, FOOTER_Y), glm::vec2(250.0f, 36.0f),
            baseLayer + 2, "Guardar cambios",
            [data]()
            {
                const bool ok = RenderSettings::instance().save()
                    && AudioManager::SaveAudioSettings();
                data->SetStatus(ok
                    ? "Ajustes guardados"
                    : "No se pudo escribir la configuracion");
            },
            SettingsWidget::Vis::Always, 0);

        MakeButton(em, glm::vec2(270.0f, FOOTER_Y), glm::vec2(250.0f, 36.0f),
            baseLayer + 2, "Volver al menu",
            []()
            {
                // Se guarda al salir: los cambios ya estaban aplicados
                // en vivo, asi que no hay nada que descartar y perderlos
                // al reiniciar solo seria una sorpresa desagradable.
                RenderSettings::instance().save();
                AudioManager::SaveAudioSettings();
                CloseSettings();
            },
            SettingsWidget::Vis::Always, 0);
    }

    // CALIDAD
    static void BuildQualityTab(EntityManager& em, SettingsPanelData* data, int baseLayer)
    {
        int row = 0;

        AddChoice(em, data, baseLayer, TAB_CALIDAD, row++, "Preset de calidad",
            SettingsUI::PresetNames(),
            []() { return static_cast<int>(RenderSettings::instance().getPreset()); },
            [data](int idx)
            {
                // setPreset reescribe todos los campos via applyPreset();
                // el resto del panel se resincroniza en el frame siguiente.
                RenderSettings::instance().setPreset(static_cast<QualityPreset>(idx));
                // El preset toca de todo, incluidos MSAA, luces y
                // anisotropia, asi que hace falta el reinit completo.
                data->pendingRenderReInit = true;
            });

        // Puro ritmo del bucle de render (ver renderLoop() en client_window.hpp):
        // se aplica en el siguiente tick, sin reinit.
        AddIntChoice(em, data, baseLayer, TAB_CALIDAD, row++, "Limite de FPS",
            { 30, 60, 90, 120, 144, 165, 240 }, "",
            []() { return RenderSettings::instance().getTargetFPS(); },
            [](int v) { RenderSettings::instance().setTargetFPS(v); });

        // Aplicado directamente sobre GLFW: el onClick corre en el hilo de
        // render (dentro de world.Update(), llamado desde Render() en
        // renderLoop()), asi que tocar la ventana aqui es seguro.
        AddChoice(em, data, baseLayer, TAB_CALIDAD, row++, "Modo de ventana",
            { "Ventana", "Sin bordes", "Pantalla completa" },
            []() { return static_cast<int>(RenderSettings::instance().getWindowMode()); },
            [](int idx)
            {
                const WindowMode mode = static_cast<WindowMode>(idx);
                RenderSettings::instance().setWindowMode(mode);
                if (OpenGLWindow* window = ClientWindow::GetWindow())
                    window->setWindowMode(mode);
            });

        // Activarla vuelve a atar el framerate al refresco del monitor,
        // por encima del pacer propio del "Limite de FPS" (ver renderLoop());
        // se deja apagada por defecto para que ese limite mande siempre.
        AddToggle(em, baseLayer, TAB_CALIDAD, row++, "VSync",
            []() { return RenderSettings::instance().getVsyncEnabled(); },
            [](bool v)
            {
                RenderSettings::instance().setVsyncEnabled(v);
                if (OpenGLWindow* window = ClientWindow::GetWindow())
                    window->setVSync(v);
            });
    }

    // SONIDO: cada slider lee/escribe directamente el volumen en bruto del
    // canal (AudioManager::*ChannelVolume ya es el estado en vivo, no hay
    // copia intermedia); "Volumen general" es el canal MASTER, que
    // AudioChannelManager::GetVolume multiplica sobre el resto al reproducir.
    static void BuildSoundTab(EntityManager& em, SettingsPanelData* data, int baseLayer)
    {
        int row = 0;

        auto addChannelSlider = [&](const std::string& label, AudioChannel channel)
            {
                AddSlider(em, baseLayer, TAB_SONIDO, row++, label,
                    0.0f, 100.0f, 1.0f, 0, "%",
                    [channel]() { return AudioManager::GetChannelVolume(channel) * 100.0f; },
                    [channel](float v) { AudioManager::SetChannelVolume(channel, v / 100.0f); });
            };

        addChannelSlider("Volumen general", AudioChannel::MASTER);
        addChannelSlider("Musica", AudioChannel::MUSIC);
        addChannelSlider("Efectos", AudioChannel::SFX);
        addChannelSlider("Voz", AudioChannel::VOICE);
        addChannelSlider("Interfaz", AudioChannel::UI);
    }

    // SOMBRAS
    static void BuildShadowsTab(EntityManager& em, SettingsPanelData* data, int baseLayer)
    {
        int row = 0;

        AddToggle(em, baseLayer, TAB_SOMBRAS, row++, "Sombras direccionales",
            []() { return RenderSettings::instance().getDirShadowsEnabled(); },
            [](bool v) { RenderSettings::instance().setDirShadowsEnabled(v); });

        AddToggle(em, baseLayer, TAB_SOMBRAS, row++, "Sombras de luces puntuales",
            []() { return RenderSettings::instance().getPointShadowsEnabled(); },
            [](bool v) { RenderSettings::instance().setPointShadowsEnabled(v); });

        // Separadas y no fusionadas: las sombras puntuales se calculan una
        // vez por cara de cubemap y por luz, mucho mas caras que la unica
        // sombra direccional, asi que conviene poder bajarlas por separado.
        AddTierChoice(em, data, baseLayer, TAB_SOMBRAS, row++, "Calidad de sombras direccionales",
            { 512, 1024, 2048, 4096, 8192 },
            []() { return RenderSettings::instance().getDirShadowResolution(); },
            [data](int v)
            {
                RenderSettings::instance().setDirShadowResolution(v);
                data->pendingShadowReInit = true;
            });

        AddTierChoice(em, data, baseLayer, TAB_SOMBRAS, row++, "Calidad de sombras puntuales",
            { 128, 256, 512, 1024, 2048 },
            []() { return RenderSettings::instance().getShadowResolution(); },
            [data](int v)
            {
                RenderSettings::instance().setShadowResolution(v);
                data->pendingShadowReInit = true;
            });
    }

    // IMAGEN (HDR / tonemapping)
    static void BuildImageTab(EntityManager& em, SettingsPanelData* data, int baseLayer)
    {
        int row = 0;

        AddSlider(em, baseLayer, TAB_IMAGEN, row++, "Exposicion",
            0.1f, 5.0f, 0.05f, 2, "",
            []() { return RenderSettings::instance().getExposure(); },
            [](float v) { RenderSettings::instance().setExposure(v); });

        AddSlider(em, baseLayer, TAB_IMAGEN, row++, "Gamma",
            1.0f, 3.0f, 0.05f, 2, "",
            []() { return RenderSettings::instance().getGamma(); },
            [](float v) { RenderSettings::instance().setGamma(v); });

        AddToggle(em, baseLayer, TAB_IMAGEN, row++, "Filmic Tonemapping",
            []() { return RenderSettings::instance().getFilmicEnabled(); },
            [](bool v) { RenderSettings::instance().setFilmicEnabled(v); });
    }

    // EFECTOS (bloom / FXAA)
    static void BuildEffectsTab(EntityManager& em, SettingsPanelData* data, int baseLayer)
    {
        int row = 0;

        AddToggle(em, baseLayer, TAB_EFECTOS, row++, "Bloom",
            []() { return RenderSettings::instance().getBloomEnabled(); },
            [](bool v) { RenderSettings::instance().setBloomEnabled(v); });

        AddSlider(em, baseLayer, TAB_EFECTOS, row++, "Bloom: intensidad",
            0.0f, 2.0f, 0.01f, 2, "",
            []() { return RenderSettings::instance().getBloomStrength(); },
            [](float v) { RenderSettings::instance().setBloomStrength(v); });

        AddToggle(em, baseLayer, TAB_EFECTOS, row++, "FXAA",
            []() { return RenderSettings::instance().getFXAAEnabled(); },
            [](bool v) { RenderSettings::instance().setFXAAEnabled(v); });
    }

    // AVANZADO (init-time: se aplican con un RenderSystem::Init())
    static void BuildAdvancedTab(EntityManager& em, SettingsPanelData* data, int baseLayer)
    {
        int row = 0;

        // Puramente runtime, sin reinit: DebugOverlaySystem lee el flag cada
        // frame directamente de RenderSettings.
        AddToggle(em, baseLayer, TAB_AVANZADO, row++, "Modo Debug (FPS / latencia)",
            []() { return RenderSettings::instance().getDebugModeEnabled(); },
            [](bool v) { RenderSettings::instance().setDebugModeEnabled(v); });

        AddTierChoice(em, data, baseLayer, TAB_AVANZADO, row++, "Cantidad de luces",
            { 64, 128, 256, 512, 1024 },
            []() { return RenderSettings::instance().getMaxLights(); },
            [data](int v)
            {
                RenderSettings::instance().setMaxLights(v);
                data->pendingRenderReInit = true;
            });

        // Sin el 0 de la lista original: apagar del todo las sombras de
        // puntuales ya es el toggle "Sombras de luces puntuales" de la
        // pestana Sombras, aqui solo interesa cuantas como maximo a la vez.
        AddTierChoice(em, data, baseLayer, TAB_AVANZADO, row++, "Cantidad de luces con sombra",
            { 1, 2, 4, 8, 16 },
            []() { return RenderSettings::instance().getMaxShadowLights(); },
            [data](int v)
            {
                RenderSettings::instance().setMaxShadowLights(v);
                data->pendingRenderReInit = true;
            });

        AddIntChoice(em, data, baseLayer, TAB_AVANZADO, row++, "Muestras MSAA",
            { 1, 2, 4, 8 }, "x",
            []() { return RenderSettings::instance().getMsaaSamples(); },
            [data](int v)
            {
                RenderSettings::instance().setMsaaSamples(v);
                data->pendingRenderReInit = true;
            });

        AddIntChoice(em, data, baseLayer, TAB_AVANZADO, row++, "Filtrado anisotropico",
            { 1, 2, 4, 8, 16 }, "x",
            []() { return static_cast<int>(RenderSettings::instance().getAnisotropy()); },
            [data](int v)
            {
                RenderSettings::instance().setAnisotropy(static_cast<float>(v));
                data->pendingRenderReInit = true;
            });
    }

    // Filas

    static float RowY(int rowIndex)
    {
        return FIRST_ROW_Y + ROW_H * static_cast<float>(rowIndex);
    }

    static void AddLabel(EntityManager& em, int baseLayer, int tab, int rowIndex,
        const std::string& label)
    {
        MakeText(em, glm::vec2(LABEL_X, RowY(rowIndex)), glm::vec2(LABEL_W, ROW_TEXT_H),
            baseLayer + 1, label, 16.0f,
            SettingsWidget::Vis::Tab, tab);
    }

    // Booleano: un boton cuyo texto sale del getter.
    static void AddToggle(EntityManager& em, int baseLayer, int tab, int rowIndex,
        const std::string& label,
        std::function<bool()> get,
        std::function<void(bool)> set)
    {
        AddLabel(em, baseLayer, tab, rowIndex, label);

        std::function<bool()> getForClick = get;

        MakeButton(em, glm::vec2(TOGGLE_X, RowY(rowIndex)), glm::vec2(TOGGLE_W, TOGGLE_H),
            baseLayer + 2, "",
            [getForClick, set]() { set(!getForClick()); },
            SettingsWidget::Vis::Tab, tab,
            [get]() { return SettingsUI::FmtBool(get()); });
    }

    // Float continuo o con paso. step = 0 deja el slider continuo.
    static void AddSlider(EntityManager& em, int baseLayer, int tab, int rowIndex,
        const std::string& label,
        float mn, float mx, float step, int decimals,
        const std::string& suffix,
        std::function<float()> get,
        std::function<void(float)> set)
    {
        AddLabel(em, baseLayer, tab, rowIndex, label);

        Entity e = em.CreateEntity();

        UIElement* el = em.AddComponent<UIElement>(e, UIElement{});
        el->anchor = UIAnchor::CENTER;
        el->position = glm::vec2(SLIDER_X, RowY(rowIndex));
        el->size = glm::vec2(SLIDER_W, SLIDER_H);
        el->pivot = glm::vec2(0.5f, 0.5f);
        el->isVisible = false;
        el->layer = baseLayer + 2;

        UISlider* slider = em.AddComponent<UISlider>(e, UISlider(mn, mx, get()));
        slider->step = step;
        slider->showValue = true;
        slider->decimals = decimals;
        slider->valueSuffix = suffix;
        slider->fontSize = 14.0f;
        // El default del componente es negro; aqui el fondo es la escena.
        slider->textColor = glm::vec4(0.95f, 0.95f, 0.95f, 1.0f);
        slider->isInteractable = false;   // el sistema lo activa por pestana
        slider->onValueChanged = std::move(set);
        slider->SetValue(get(), false);   // re-snap con el step ya asignado

        Tag(em, e, SettingsWidget::Vis::Tab, tab, baseLayer + 2,
            nullptr, std::move(get), nullptr);
    }

    // Lista de opciones con nombre. getIndex/setIndex trabajan con indices.
    static void AddChoice(EntityManager& em, SettingsPanelData* data,
        int baseLayer, int tab, int rowIndex,
        const std::string& label,
        const std::vector<std::string>& names,
        std::function<int()> getIndex,
        std::function<void(int)> setIndex)
    {
        AddLabel(em, baseLayer, tab, rowIndex, label);

        Entity e = em.CreateEntity();

        UIElement* el = em.AddComponent<UIElement>(e, UIElement{});
        el->anchor = UIAnchor::CENTER;
        el->position = glm::vec2(DROPDOWN_X, RowY(rowIndex));
        el->size = glm::vec2(DROPDOWN_W, DROPDOWN_H);
        el->pivot = glm::vec2(0.5f, 0.5f);
        el->isVisible = false;
        el->layer = baseLayer + 2;

        UIDropdown* dd = em.AddComponent<UIDropdown>(e, UIDropdown("Seleccionar..."));
        dd->SetOptions(names);
        dd->fontSize = 15.0f;
        dd->itemHeight = 26.0f;
        dd->maxVisibleItems = 6;
        dd->isInteractable = false;       // el sistema lo activa por pestana
        dd->SelectIndex(getIndex(), false);
        dd->onSelectionChanged =
            [setIndex](int index, const std::string&) { setIndex(index); };

        Tag(em, e, SettingsWidget::Vis::Tab, tab, baseLayer + 2,
            nullptr, nullptr, std::move(getIndex));
    }

    // Lista de enteros. Traduce valor <-> indice, tolerando valores que no
    // esten en la lista (los presets escriben lo que quieren).
    static void AddIntChoice(EntityManager& em, SettingsPanelData* data,
        int baseLayer, int tab, int rowIndex,
        const std::string& label,
        const std::vector<int>& options,
        const std::string& suffix,
        std::function<int()> get,
        std::function<void(int)> set)
    {
        // Copia por valor: los lambdas viven mas que este ambito.
        const std::vector<int> opts = options;

        AddChoice(em, data, baseLayer, tab, rowIndex, label,
            SettingsUI::IntOptionNames(opts, suffix),
            [opts, get]() { return SettingsUI::IndexOfNearest(opts, get()); },
            [opts, set](int index)
            {
                if (index >= 0 && index < static_cast<int>(opts.size()))
                    set(opts[index]);
            });
    }

    // Como AddIntChoice, pero con nombres de nivel (SettingsUI::TierNames())
    // en vez del numero crudo: para ajustes que un jugador casual debe poder
    // tocar sin saber que significa "2048" o "256". options debe tener el
    // mismo tamano que TierNames().
    static void AddTierChoice(EntityManager& em, SettingsPanelData* data,
        int baseLayer, int tab, int rowIndex,
        const std::string& label,
        const std::vector<int>& options,
        std::function<int()> get,
        std::function<void(int)> set)
    {
        const std::vector<int> opts = options;

        AddChoice(em, data, baseLayer, tab, rowIndex, label,
            SettingsUI::TierNames(),
            [opts, get]() { return SettingsUI::IndexOfNearest(opts, get()); },
            [opts, set](int index)
            {
                if (index >= 0 && index < static_cast<int>(opts.size()))
                    set(opts[index]);
            });
    }

    // Primitivas

    static void Tag(EntityManager& em, Entity e,
        SettingsWidget::Vis vis, int tab, int normalLayer,
        std::function<std::string()> readText = nullptr,
        std::function<float()> readSlider = nullptr,
        std::function<int()> readChoice = nullptr)
    {
        SettingsWidget* w = em.AddComponent<SettingsWidget>(e, SettingsWidget{});
        w->vis = vis;
        w->tab = tab;
        w->normalLayer = normalLayer;
        w->readText = std::move(readText);
        w->readSlider = std::move(readSlider);
        w->readChoice = std::move(readChoice);
    }

    static Entity MakeText(EntityManager& em,
        glm::vec2 center, glm::vec2 size, int layer,
        const std::string& initial, float fontSize,
        SettingsWidget::Vis vis, int tab,
        std::function<std::string()> read = nullptr,
        glm::vec4 color = glm::vec4(1.0f))
    {
        Entity e = em.CreateEntity();

        UIElement* el = em.AddComponent<UIElement>(e, UIElement{});
        el->anchor = UIAnchor::CENTER;
        el->position = center;
        el->size = size;
        el->pivot = glm::vec2(0.5f, 0.5f);
        el->isVisible = false;
        el->layer = layer;

        UIText* txt = em.AddComponent<UIText>(e, UIText{});
        txt->text = initial;
        txt->fontSize = fontSize;
        txt->SetColor(color.r, color.g, color.b, color.a);
        txt->SetFont("default");

        Tag(em, e, vis, tab, layer, std::move(read), nullptr, nullptr);
        return e;
    }

    static Entity MakeButton(EntityManager& em,
        glm::vec2 center, glm::vec2 size, int layer,
        const std::string& label,
        std::function<void()> onClick,
        SettingsWidget::Vis vis, int tab,
        std::function<std::string()> dynamicLabel = nullptr)
    {
        Entity e = em.CreateEntity();

        UIElement* el = em.AddComponent<UIElement>(e, UIElement{});
        el->anchor = UIAnchor::CENTER;
        el->position = center;
        el->size = size;
        el->pivot = glm::vec2(0.5f, 0.5f);
        el->isVisible = false;
        el->layer = layer;

        UIButton* btn = em.AddComponent<UIButton>(e);
        btn->text = label;
        btn->isInteractable = false;   // el sistema lo activa segun la pestana
        btn->onClick = std::move(onClick);

        Tag(em, e, vis, tab, layer, std::move(dynamicLabel), nullptr, nullptr);
        return e;
    }
};

// La escena, modelada sobre StartScreenGame. La logica esta vacia: los ajustes son estado local.

struct SettingsSceneState {
    int frameCount;
};

class SettingsScreenGame : public IECSGameLogic {
public:
    std::unique_ptr<IGameLogic> Clone() const override {
        return std::make_unique<SettingsScreenGame>();
    }

    InputBlob GenerateLocalInput() override {
        // El panel se maneja entero con el raton, via callbacks de UI.
        return MakeZeroInputBlob();
    }

    void GameState_To_ECSWorld(const GameStateBlob& state) override {
        // Nada que sincronizar: el panel vive solo en el renderer.
    }

    void ECSWorld_To_GameState(GameStateBlob& state) override {
        SettingsSceneState& s = *reinterpret_cast<SettingsSceneState*>(state.data);
        s.frameCount++;
        state.len = sizeof(SettingsSceneState);
    }

    bool CompareStates(const GameStateBlob& a, const GameStateBlob& b) const override {
        return std::memcmp(a.data, b.data, sizeof(SettingsSceneState)) == 0;
    }

    void InitECSLogic(GameStateBlob& state) override {
        SettingsSceneState* s = reinterpret_cast<SettingsSceneState*>(state.data);
        s->frameCount = 0;
        state.len = sizeof(SettingsSceneState);
    }

    void HashState(const GameStateBlob& state, uint8_t(&outHash)[SHA256_DIGEST_LENGTH]) const override {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) throw std::runtime_error("Failed to create EVP_MD_CTX");

        if (1 != EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestInit_ex failed");
        }

        // Sin datos que hashear: los ajustes no son estado de juego.

        unsigned int len = 0;
        if (1 != EVP_DigestFinal_ex(ctx, outHash, &len)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestFinal_ex failed");
        }

        EVP_MD_CTX_free(ctx);
    }

    void PrintState(const GameStateBlob& state) const override {
        SettingsSceneState s;
        std::memcpy(&s, state.data, sizeof(SettingsSceneState));
        printf("=== Settings Screen State ===\n");
        printf("Frame Count: %d\n", s.frameCount);
        printf("=============================\n");
    }
};

class SettingsScreenGameRenderer : public IECSGameRenderer {
public:
    void GameState_To_ECSWorld(const GameStateBlob& state) override {
        // El panel se refresca solo desde RenderSettings en SettingsPanelSystem.
    }

    void InitECSRenderer(const GameStateBlob& state, OpenGLWindow* window) override {
        this->window = window;

        EntityManager& em = world.GetEntityManager();

        SettingsPanel::Register(em);

        // Camara ortografica, igual que la del menu.
        Entity camera = em.CreateEntity();
        Transform* camTrans = em.AddComponent<Transform>(camera, Transform{});
        camTrans->setPosition(glm::vec3(0.0f, 0.0f, 2.0f));
        Camera* camSettings = em.AddComponent<Camera>(camera, Camera{});
        camSettings->setOrthographic(-100.0f, 100.0f, -100.0f, 100.0f, 0.1f, 100.0f);
        camSettings->setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
        camSettings->setUp(glm::vec3(0.0f, 1.0f, 0.0f));

        SettingsPanel::Build(em);
        world.AddSystem(std::make_unique<SettingsPanelSystem>());

        // Esta escena no manda nada a la logica: los ajustes no son estado de
        // juego. Por eso no se asigna renderDataTransferToLogicCallback.
    }

    void Interpolate(const GameStateBlob& previousServerState,
        const GameStateBlob& currentServerState,
        const GameStateBlob& previousLocalState,
        const GameStateBlob& currentLocalState,
        GameStateBlob& renderState,
        float serverInterpolation,
        float localInterpolation) override {

        const SettingsSceneState& currServer =
            *reinterpret_cast<const SettingsSceneState*>(currentServerState.data);
        SettingsSceneState& rend = *reinterpret_cast<SettingsSceneState*>(renderState.data);
        rend = currServer;
    }

    ~SettingsScreenGameRenderer() override {
    }

private:
    OpenGLWindow* window = nullptr;
};


#endif // SETTINGS_SCENE_HPP