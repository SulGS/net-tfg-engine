#pragma once

#include <algorithm>
#include <unordered_set>

#include "ecs/ecs_common.hpp"
#include "../Components.hpp"

// Drives tile/wall/spoke/pillar visibility from replicated state, and each wall's beam animation (laser_wall.frag)
// easing toward solid/warning/off via LaserWallVisual. Purely render-side, never synced or predicted.
class LaserWallRenderSystem : public ISystem
{
    // Quick to energise so the wall still reads as a snap; slower to die so the
    // beam visibly collapses instead of vanishing.
    const float POWER_RISE_TIME = 0.20f;
    const float POWER_FALL_TIME = 0.30f;
    const float WARNING_BLEND_TIME = 0.15f;  // fade to/from the amber look
    const float WARNING_POWER = 0.5f;        // width/brightness of the amber preview beam
    const float WARNING_RAMP_TIME = 3.0f;    // = WARNING_THRESHOLD in ArenaSystem.hpp; how long the stutter takes to go from mostly-off to mostly-on
    const float FLASH_DECAY_TIME = 0.35f;

    float time = 0.0f; // render-side clock for the beam shader ("uTime")

    static float MoveToward(float current, float target, float maxDelta)
    {
        if (current < target) return std::min(current + maxDelta, target);
        return std::max(current - maxDelta, target);
    }

    // Eases one wall/spoke toward what the replicated state says (solid /
    // warning / off), toggles its mesh, and pushes the result to the shader.
    void AnimateWall(MeshComponent* mesh, LaserWallVisual* vis, bool solid, bool warning, float deltaTime)
    {
        // Just energised: white-hot burst that decays.
        if (solid && !vis->wasSolid) vis->flash = 1.0f;
        vis->wasSolid = solid;
        vis->flash = std::max(0.0f, vis->flash - deltaTime / FLASH_DECAY_TIME);

        const float targetPower = solid ? 1.0f : (warning ? WARNING_POWER : 0.0f);
        const float riseFall = (targetPower > vis->power) ? POWER_RISE_TIME : POWER_FALL_TIME;
        vis->power = MoveToward(vis->power, targetPower, deltaTime / riseFall);

        vis->warning = MoveToward(vis->warning, warning ? 1.0f : 0.0f, deltaTime / WARNING_BLEND_TIME);
        vis->warnTime = warning ? vis->warnTime + deltaTime : 0.0f;

        // Fully faded out: stop drawing it altogether (also skips the uniform uploads).
        mesh->enabled = vis->power > 0.002f;
        if (!mesh->enabled || !mesh->mesh) return;

        if (Material* mat = mesh->mesh->getMaterial())
        {
            mat->setFloat("uTime", time);
            mat->setFloat("uIntensity", vis->power);
            mat->setFloat("uWarning", vis->warning);
            mat->setFloat("uWarnTension", std::min(vis->warnTime / WARNING_RAMP_TIME, 1.0f));
            mat->setFloat("uFlash", vis->flash);
        }
    }

public:
    void Update(
        EntityManager& entityManager,
        std::vector<EventEntry>& events,
        bool isServer,
        float deltaTime
    ) override
    {
        const int x_size = 5;
        const int y_size = 5;

        time += deltaTime;

        // Build active tile set
        std::unordered_set<int> activeTileIds;
        {
            auto tileActiveQuery = entityManager.CreateQuery<TileID>();
            for (auto [entity, tileId] : tileActiveQuery)
                if (tileId->active) activeTileIds.insert(tileId->id);
        }

        // Tiles: active flag drives visibility, warning drives fall
        {
            auto tileQuery = entityManager.CreateQuery<TileID, MeshComponent, Transform>();
            for (auto [entity, tileId, mesh, transform] : tileQuery)
            {
                mesh->enabled = tileId->active;

                if (tileId->active && tileId->warning)
                {
                    tileId->warningFallAccum += deltaTime;
                    float drop = tileId->warningFallAccum * tileId->warningFallAccum * 6.0f;
                    glm::vec3 pos = transform->getPosition();
                    transform->setPosition(glm::vec3(pos.x, pos.y, -4.0f - drop));
                }
                else if (tileId->active && !tileId->warning)
                {
                    tileId->warningFallAccum = 0.0f;
                    glm::vec3 pos = transform->getPosition();
                    transform->setPosition(glm::vec3(pos.x, pos.y, -4.0f));
                }
            }
        }

        // Walls and spokes
        {
            auto laserWallQuery = entityManager.CreateQuery<LaserWallID, MeshComponent, LaserWallVisual>();
            for (auto [entity, lwID, mesh, visual] : laserWallQuery)
            {
                bool isSpoke = entityManager.GetComponent<CenterSpoke>(entity) != nullptr;

                // solid = beam should be energised; warning = about to be
                // (disabled but flagged). Same rules as before, only the
                // result now feeds the animation instead of mesh->enabled.
                bool solid = false;
                bool warning = false;

                if (isSpoke)
                {
                    // Single-owner: no neighbour concept, only its own cell matters.
                    if (activeTileIds.count(lwID->cellId))
                    {
                        solid = lwID->enabled;
                        warning = lwID->warning && !lwID->enabled;
                    }
                }
                else
                {
                    // Shared edge: one entity per boundary, stored under whichever cell was visited first, so visibility
                    // must be judged symmetrically — see ClassifyWallEdge.
                    WallEdgeState edgeState = ClassifyWallEdge(lwID->cellId, lwID->dir, activeTileIds);

                    if (edgeState == WallEdgeState::SoleBorder)
                    {
                        solid = true;
                    }
                    else if (edgeState == WallEdgeState::Interior)
                    {
                        solid = lwID->enabled;
                        warning = lwID->warning && !lwID->enabled;
                    }
                }

                AnimateWall(mesh, visual, solid, warning, deltaTime);
            }
        }

        // Pillars: hidden when ALL their cells are inactive
        {
            auto pillarQuery = entityManager.CreateQuery<PillarID, MeshComponent>();
            for (auto [entity, pid, mesh] : pillarQuery)
            {
                if (pid->cellsIsIn.empty()) { mesh->enabled = false; continue; }
                bool allGone = true;
                for (int cellId : pid->cellsIsIn)
                    if (activeTileIds.count(cellId)) { allGone = false; break; }
                mesh->enabled = !allGone;
            }
        }
    }
};
