#pragma once

#include "ecs/ecs.hpp"
#include "ecs/ecs_common.hpp"
#include "../Components.hpp"
#include <unordered_map>
#include <vector>
#include <cmath>

// Reflection probes are captured once (RenderSystem::EnvironmentPass), so what they reflect is the arena as it was at
// that moment. When a tile drops out or comes back (and the pillars that depend on it with it), the probes close
// enough to see it are re-captured. Runs before RenderSystem in the same frame, after LaserWallRenderSystem has hidden
// the tile's mesh, so the new capture already leaves it out.
class ReflectionProbeRefreshSystem : public ISystem {
public:
    // A tile counts for probes whose cell centre is within this (x/y, Chebyshev) of it: the probe's own cell and the
    // 8 around it. Farther tiles are a few texels in the capture.
    static constexpr float kReach = 120.0f;

    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override
    {
        std::vector<glm::vec3> changed;
        {
            auto tileQuery = entityManager.CreateQuery<TileID, Transform>();
            for (auto [entity, tile, transform] : tileQuery) {
                auto it = m_lastActive.find(entity);
                if (it == m_lastActive.end()) {
                    m_lastActive.emplace(entity, tile->active);
                    continue;
                }
                if (it->second != tile->active) {
                    it->second = tile->active;
                    changed.push_back(transform->getPosition());
                }
            }
        }
        if (changed.empty()) return;

        auto probeQuery = entityManager.CreateQuery<ReflectionProbeComponent, Transform>();
        for (auto [entity, probe, transform] : probeQuery) {
            const glm::vec3 p = transform->getPosition();
            for (const glm::vec3& tile : changed) {
                if (std::abs(tile.x - p.x) <= kReach && std::abs(tile.y - p.y) <= kReach) {
                    probe->recapture = true;
                    break;
                }
            }
        }
    }

private:
    std::unordered_map<Entity, bool> m_lastActive;
};
