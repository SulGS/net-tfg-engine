#pragma once

#include <memory>
#include <glm/glm.hpp>

#include "ecs/ecs_common.hpp"
#include "OpenGL/Material.hpp"
#include "OpenGL/Mesh.hpp"

// ---- Laser shot + charge-up visuals ---- Volumetric glows raymarched inside a UNIT SPHERE canvas (charge.glb), drawn
// additively; the entity scale stretches it into a long ellipsoid (bolt) or a sphere (orb). See glow_volume.vert.
inline constexpr const char* GLOW_VOLUME_MESH = "charge.glb";

// Bolt canvas: local +X = flight direction. The head sits ~0.7 ahead of the bullet position (on its 2x2 box) and the
// tail trails ~7 units, a bit more than a bullet's 5 units per tick, so the streak always covers the last tick's travel.
inline const glm::vec3 LASER_BOLT_SCALE(7.0f, 1.7f, 1.7f);

// Light a bolt casts on what it flies over; one of the few point lights, so it does the visible work. At ~4 above
// the tiles: floor right under it ~3x albedo, ~10 units away ~0.4x, nothing beyond the radius.
inline constexpr float LASER_BOLT_LIGHT_INTENSITY = 150.0f;
inline constexpr float LASER_BOLT_LIGHT_RADIUS = 40.0f;
inline const glm::vec3 LASER_BOLT_LIGHT_COLOR(1.0f, 0.6f, 0.2f);

// Charge orb canvas radius (the visible glow is smaller). Its distance ahead of the ship is SHIP_MUZZLE_OFFSET
// (Components.hpp), shared with InputServerSystem so the bolt starts where the orb was.
inline constexpr float CHARGE_ORB_RADIUS = 4.2f;

// Adds the bolt mesh to a bullet. One Material per bolt (own age/fade uniforms, see BulletRenderSystem), program
// shared via ShaderLoader's cache. Colours are light ADDED to the scene, not a surface colour.
inline MeshComponent* AddLaserBoltMesh(EntityManager& em, Entity bullet, int bulletId)
{
    auto mat = std::make_shared<Material>("glow_volume.vert", "laser_bolt.frag");
    mat->setVec3("uColor", glm::vec3(1.0f, 0.55f, 0.08f));
    mat->setVec3("uHotColor", glm::vec3(1.0f, 0.92f, 0.6f));
    mat->setFloat("uGlowStrength", 1.8f);
    mat->setFloat("uCoreStrength", 3.2f);
    mat->setFloat("uSeed", bulletId * 2.7f); // desyncs the tail noise between bolts

    MeshComponent* mc = em.AddComponent<MeshComponent>(bullet,
        MeshComponent(new Mesh(GLOW_VOLUME_MESH, mat)));
    mc->castShadows = false;
    mc->additive = true;
    return mc;
}

// Adds the bolt's point light (follows the Transform; BulletRenderSystem scales intensity by age/fade, so it starts at 0).
// No shadows on purpose: shadow cube-map slots are few (8) and expensive, and PointLightComponent casts them by default.
inline PointLightComponent* AddLaserBoltLight(EntityManager& em, Entity bullet)
{
    PointLightComponent* light =
        em.AddComponent<PointLightComponent>(bullet, PointLightComponent{});
    light->color = LASER_BOLT_LIGHT_COLOR;
    light->intensity = 0.0f;
    light->radius = LASER_BOLT_LIGHT_RADIUS;
    light->castShadows = true;
    return light;
}

// Same for the charge-up orb of a ship (seed = its player id).
inline MeshComponent* AddChargeOrbMesh(EntityManager& em, Entity orb, int playerId)
{
    auto mat = std::make_shared<Material>("glow_volume.vert", "laser_charge.frag");
    mat->setVec3("uColor", glm::vec3(1.0f, 0.55f, 0.08f));
    mat->setVec3("uHotColor", glm::vec3(1.0f, 0.92f, 0.6f));
    mat->setFloat("uGlowStrength", 1.8f);
    mat->setFloat("uCoreStrength", 3.2f);
    mat->setFloat("uSeed", playerId * 3.1f);

    MeshComponent* mc = em.AddComponent<MeshComponent>(orb,
        MeshComponent(new Mesh(GLOW_VOLUME_MESH, mat)));
    mc->castShadows = false;
    mc->additive = true;
    return mc;
}
