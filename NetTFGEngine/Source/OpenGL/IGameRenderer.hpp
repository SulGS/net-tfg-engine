#ifndef IGameRenderer_hpp
#define IGameRenderer_hpp

#include "netcode/netcode_common.hpp"
#include "OpenGL/OpenGLWindow.hpp"
#include "OpenGL/Mesh.hpp"
#include "Utils/LoadPlan.hpp"
#include "Utils/LoadingScreenStyle.hpp"

#include <memory>

class IGameRenderer {
private:

	IGameLogic* gameLogic = nullptr;

public:

    int playerId = -1;

    virtual void Init(const GameStateBlob& state, OpenGLWindow* window) = 0;

    // What the render loop actually runs to set this renderer up: Init() cut into steps that it spreads over frames,
    // with the loading screen on top (see LoadPlan). The default is Init() as a single step. `state` is only valid
    // during this call: steps that need it keep a copy.
    virtual void BuildLoadPlan(LoadPlan& plan, const GameStateBlob& state, OpenGLWindow* window) {
        auto initial = std::make_shared<GameStateBlob>(state);
        plan.Add(GetLoadingScreenStyle().sceneSetupLabel, 1.0f, [this, initial, window]() { Init(*initial, window); });
    }

    void LinkGameLogic(IGameLogic* logic) {
        gameLogic = logic;
    }

    IGameLogic* GetGameLogic() {
		return gameLogic;
	}

    virtual void Render(const GameStateBlob& state, OpenGLWindow* window) = 0;

    virtual void Interpolate(const GameStateBlob& previousServerState, const GameStateBlob& currentServerState, const GameStateBlob& previousLocalState, const GameStateBlob& currentLocalState, GameStateBlob& renderState, float serverInterpolation, float localInterpolation) = 0;

    // Releases AssetManager-backed resources held by this renderer's components; no-op by default, must run on the render thread.
    virtual void ReleaseECSAssets() {}

    virtual ~IGameRenderer() = 0;

};

inline IGameRenderer::~IGameRenderer() {}

#endif /* IGameRenderer_hpp */