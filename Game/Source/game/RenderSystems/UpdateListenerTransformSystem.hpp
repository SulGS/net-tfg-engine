#pragma once

#include "ecs/ecs_common.hpp"
#include "OpenAL/AudioComponents.hpp"
#include "../Components.hpp"

// Puts the audio listener where the camera looks, facing the way the screen does. Must run after CameraFollowSystem
// (and MatchStartCountdownRenderSystem) so it uses this frame's camera target.
//
// Position: the camera target, not the local ship, so spectating, or the game-over zoom on the winner, is heard from
// whoever is on screen instead of from where the local ship died.
// Orientation: fixed to the screen, never the ship's. The camera doesn't rotate (it looks from -Y toward +Y, +Z up), so
// copying the ship's yaw made a blast on the right of the screen sound behind or on the left whenever the ship
// faced anywhere but screen-right. AudioSystem takes the listener's local +X as forward and +Z as up: a 90 deg yaw
// turns forward to +Y (top of the screen), leaving +X (screen right) on the listener's right.
class UpdateListenerTransformSystem : public ISystem
{
public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        auto camQuery = entityManager.CreateQuery<Camera, Transform>();
        const Camera* camera = nullptr;
        for (auto [camEntity, cam, camTrans] : camQuery)
        {
            camera = cam;
            break;
        }
        if (!camera) return;

        auto listenerQuery = entityManager.CreateQuery<Transform, AudioListenerComponent>();
        for (auto [listenerEntity, listenerTransform, listener] : listenerQuery)
        {
            listenerTransform->setPosition(camera->getTarget());
            listenerTransform->setRotation(glm::vec3(0.0f, 0.0f, 90.0f));
        }
    }
};
