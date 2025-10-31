#ifndef IGameRenderer_hpp
#define IGameRenderer_hpp

#include "netcode/netcode_common.hpp"
#include "OpenGL/OpenGLWindow.hpp"
#include "OpenGL/Mesh.hpp"

class IGameRenderer {
public:

    int playerId = -1;

    virtual void Init(const GameStateBlob& state, OpenGLWindow* window) = 0;

    virtual void Render(const GameStateBlob& state, OpenGLWindow* window) = 0;

    virtual void Interpolate(const GameStateBlob& previousState, const GameStateBlob& currentState, GameStateBlob& renderState, float interpolationFactor) = 0;
    
    virtual ~IGameRenderer() = 0;

};

inline IGameRenderer::~IGameRenderer() {}

#endif /* IGameRenderer_hpp */