#include "netcode/netcode_common.hpp"
#include "GameState.hpp"

enum AsteroidsDeltaTypes {
    DELTA_GAME_POSITIONS = 0
};

struct GamePositionsDelta {
    float posX[2];
    float posY[2];
};

static bool CheckGamePositionsDelta(const GameStateBlob& state, const DeltaStateBlob& delta) 
{
    GamePositionsDelta gpd = *reinterpret_cast<const GamePositionsDelta*>(delta.data);
    AsteroidShooterGameState gs = *reinterpret_cast<const AsteroidShooterGameState*>(state.data);

    for (int i = 0; i < 2; i++) 
    {
        if (gs.posX[i] != gpd.posX[i]) return false;
        if (gs.posY[i] != gpd.posY[i]) return false;
    }

    return true;
}