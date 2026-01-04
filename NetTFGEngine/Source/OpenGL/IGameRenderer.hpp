#ifndef IGameRenderer_hpp
#define IGameRenderer_hpp

#include "netcode/netcode_common.hpp"
#include "OpenGL/OpenGLWindow.hpp"
#include "OpenGL/Mesh.hpp"

class IGameRenderer {
private:

	IGameLogic* gameLogic;

public:

    int playerId = -1;

    virtual void Init(const GameStateBlob& state, OpenGLWindow* window) = 0;

    void LinkGameLogic(IGameLogic* logic) {
        gameLogic = logic;
    }

    IGameLogic* GetGameLogic() {
		return gameLogic;
	}

    virtual void Render(const GameStateBlob& state, OpenGLWindow* window) = 0;

    virtual void Interpolate(const GameStateBlob& previousServerState, const GameStateBlob& currentServerState, const GameStateBlob& previousLocalState, const GameStateBlob& currentLocalState, GameStateBlob& renderState, float serverInterpolation, float localInterpolation) = 0;
    
    virtual ~IGameRenderer() = 0;

};

inline IGameRenderer::~IGameRenderer() {}

#endif /* IGameRenderer_hpp */