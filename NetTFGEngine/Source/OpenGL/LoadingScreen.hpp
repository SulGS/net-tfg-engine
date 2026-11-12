#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "OpenGL/OpenGLIncludes.hpp"
#include "OpenGL/OpenGLWindow.hpp"
#include "ecs/ecs.hpp"
#include "ecs/UI/UIElement.hpp"
#include "ecs/UI/UIImage.hpp"
#include "ecs/UI/UIText.hpp"
#include "ecs/UI/UIRenderSystem.hpp"
#include "ecs/UI/DefaultFont.hpp"
#include "Utils/LoadingScreenStyle.hpp"
#include "Utils/Debug/Debug.hpp"

// Full-screen loading screen drawn by ClientWindow::renderLoop() over (or instead of) the scene while a scene loads:
// title, progress bar, current step and a moving highlight that keeps the screen visibly alive during a long step.
// Texts, colours, sizes and font come from the game's LoadingScreenStyle, read every frame. Lives outside every scene
// (own EntityManager and UIRenderSystem) so it can draw while the scene's world is half built. Render thread only;
// Release() before the GL context goes away.
class LoadingScreen {
public:
    // A new load: the bar starts again from 0 (it never moves backwards within one load).
    void Begin() {
        shownProgress_ = 0.0f;
        time_ = 0.0f;
    }

    // showContent false: only the background (the first moments of a load, which are usually too short to be worth a
    // flash of text, still get a defined frame instead of whatever the back buffer held).
    void Draw(OpenGLWindow* window, float progress, const std::string& label, bool showContent, float dt) {
        if (!window || window->isMinimized()) return;

        const LoadingScreenStyle style = GetLoadingScreenStyle();
        if (!uir_ || style.fontPath != builtFontPath_) Build(style);

        dt = std::clamp(dt, 0.0f, 0.1f);
        time_ += dt;
        // Eased toward the target, never backwards: steps finish in bursts and weights are rough.
        const float target = std::clamp(progress, 0.0f, 1.0f);
        if (target > shownProgress_)
            shownProgress_ = std::min(target, shownProgress_ + std::max((target - shownProgress_) * std::min(1.0f, dt * 10.0f), dt * 0.05f));

        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window->getWindow(), &fbW, &fbH);
        if (fbW <= 0 || fbH <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fbW, fbH);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(style.backgroundColor.r, style.backgroundColor.g, style.backgroundColor.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (!showContent) return;

        UpdateWidgets(style, label);
        uir_->UpdateScreenSize(window->getLogicalWidth(), window->getLogicalHeight());
        std::vector<EventEntry> events;
        uir_->Update(*em_, events, false, dt);
    }

    void Release() {
        uir_.reset();
        em_.reset();
    }

private:
    static constexpr float kRefW = 1920.0f;
    static constexpr float kRefH = 1080.0f;
    static constexpr float kSweepPeriodS = 1.6f;

    std::unique_ptr<EntityManager> em_;
    std::unique_ptr<UIRenderSystem> uir_;
    std::string builtFontPath_;
    Entity title_ = 0, barBack_ = 0, barFill_ = 0, sweep_ = 0, label_ = 0, percent_ = 0;

    float shownProgress_ = 0.0f;
    float time_ = 0.0f;

    Entity AddElement(int layer) {
        Entity e = em_->CreateEntity();
        UIElement* el = em_->AddComponent<UIElement>(e, UIElement{});
        el->anchor = UIAnchor::CENTER;
        el->layer = layer;
        return e;
    }

    // Solid quad: UIImage with no texture (isLoaded so the renderer doesn't try to load one).
    Entity AddQuad(int layer) {
        Entity e = AddElement(layer);
        UIImage* img = em_->AddComponent<UIImage>(e, UIImage{});
        img->isLoaded = true;
        img->textureID = 0;
        return e;
    }

    Entity AddText(UITextAlign align) {
        Entity e = AddElement(10);
        UIText* t = em_->AddComponent<UIText>(e, UIText{});
        t->align = align;
        t->SetFont("default");
        return e;
    }

    void Build(const LoadingScreenStyle& style) {
        Release();
        em_ = std::make_unique<EntityManager>();
        em_->RegisterComponentType<UIElement>();
        em_->RegisterComponentType<UIImage>();
        em_->RegisterComponentType<UIText>();

        uir_ = std::make_unique<UIRenderSystem>((int)kRefW, (int)kRefH);
        builtFontPath_ = style.fontPath;
        bool fontLoaded = false;
        if (!style.fontPath.empty()) {
            fontLoaded = uir_->LoadFont("default", style.fontPath, 32, true);
            if (!fontLoaded)
                Debug::Warning("LoadingScreen") << "Can't load the style's font '" << style.fontPath << "', using the default\n";
        }
        if (!fontLoaded) LoadDefaultUIFont(uir_.get());

        title_   = AddText(UITextAlign::CENTER);
        barBack_ = AddQuad(1);
        barFill_ = AddQuad(2);
        sweep_   = AddQuad(3);
        label_   = AddText(UITextAlign::LEFT);
        percent_ = AddText(UITextAlign::RIGHT);
    }

    void Place(Entity e, glm::vec2 pos, glm::vec2 size, glm::vec2 pivot) {
        UIElement* el = em_->GetComponent<UIElement>(e);
        el->position = pos;
        el->size = size;
        el->pivot = pivot;
    }

    void SetQuad(Entity e, const glm::vec4& color, bool visible = true) {
        em_->GetComponent<UIImage>(e)->color = color;
        em_->GetComponent<UIElement>(e)->isVisible = visible;
    }

    void SetText(Entity e, const std::string& text, float size, const glm::vec4& color) {
        UIText* t = em_->GetComponent<UIText>(e);
        t->text = text;
        t->fontSize = size;
        t->color = color;
    }

    // Layout and look from the style every frame: cheap, and a style changed mid-load applies at once.
    void UpdateWidgets(const LoadingScreenStyle& style, const std::string& label) {
        const float barW = std::max(style.barWidth, 1.0f);
        const float barH = std::max(style.barHeight, 1.0f);
        const float barLeft = -barW * 0.5f;
        const float textH = style.textSize * 1.5f;
        const float titleH = style.titleSize * 1.4f;
        const float gap = std::max(style.textSize * 0.6f, 12.0f);

        Place(title_, { 0.0f, -(barH * 0.5f + gap + titleH * 0.5f) }, { barW, titleH }, { 0.5f, 0.5f });
        SetText(title_, style.title, style.titleSize, style.titleColor);

        Place(barBack_, { 0.0f, 0.0f }, { barW, barH }, { 0.5f, 0.5f });
        SetQuad(barBack_, style.barBackColor);

        Place(barFill_, { barLeft, 0.0f }, { barW * shownProgress_, barH }, { 0.0f, 0.5f });
        SetQuad(barFill_, style.barFillColor);

        // Back and forth along the bar, eased at the ends.
        const float sweepW = std::min(barW * 0.16f, barW);
        const float phase = std::fmod(time_ / kSweepPeriodS, 2.0f);
        const float tri = phase < 1.0f ? phase : 2.0f - phase;
        const float eased = tri * tri * (3.0f - 2.0f * tri);
        Place(sweep_, { barLeft + eased * (barW - sweepW), 0.0f }, { sweepW, barH }, { 0.0f, 0.5f });
        SetQuad(sweep_, style.sweepColor, style.showSweep);

        const glm::vec2 textPos{ 0.0f, barH * 0.5f + gap + textH * 0.5f };
        const int dots = style.animatedDots ? int(time_ * 2.5f) % 4 : 0;
        Place(label_, textPos, { barW, textH }, { 0.5f, 0.5f });
        SetText(label_, label + std::string(dots, '.'), style.textSize, style.textColor);

        Place(percent_, textPos, { barW, textH }, { 0.5f, 0.5f });
        SetText(percent_, style.showPercent ? std::to_string(int(shownProgress_ * 100.0f + 0.5f)) + " %" : std::string(),
            style.textSize, style.textColor);
    }
};
