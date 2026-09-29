#pragma once
// GBUFFER VARIANT of a material's fragment shader. By default GBufferPass draws every mesh with the engine's own
// GBuffer shader (normal map + MR map), which knows nothing about custom materials: vertex displacement, procedural
// normals or roughness computed in the shader never reach SSAO/SSR. A fragment shader opts in by handling GBUFFER_PASS:
//
//     #ifndef GBUFFER_PASS
//     layout(location = 0) out vec4 FragColor;   // the GBuffer owns locations 0-3 in the variant
//     #endif
//     ...
//     void main() {
//         ... evaluate the surface: N (world space), roughness, metallic, albedo ...
//     #ifdef GBUFFER_PASS
//         WriteGBuffer(N, roughness, metallic, albedo, false);
//     #else
//         ... lighting, FragColor = ...
//     #endif
//     }
//
// Material compiles that variant alongside the normal program, with the same vertex shader (so displacement matches)
// and the same uniform values; GBufferPass then uses it for that mesh. Keep the lighting out of the GBUFFER_PASS branch:
// samplers it would reference aren't bound for the GBuffer pass.
//
// Motion blur: WriteGBuffer() reports no object motion, so such meshes blur with the camera but not with their own
// movement (fine for the static fluid floors that use it today).

namespace GBufferVariant
{
    inline constexpr const char* Define = "GBUFFER_PASS";

    // Injected right after the fragment shader's #version line. Layout must match RenderSystem::InitGBufferFBO().
    inline constexpr const char* Preamble = R"GLSL(
#define GBUFFER_PASS 1
layout(location = 0) out vec4 gbNormalRoughness; // RGBA16F: xyz = view-space normal, w = perceptual roughness
layout(location = 1) out vec4 gbMaterial;        // RGBA8:   r = metalness, g = 1 if the shader has its own env reflection
layout(location = 2) out vec4 gbAlbedo;          // RGBA8:   rgb = base colour
layout(location = 3) out vec2 gbObjectVelocity;  // RG16F:   object motion for motion blur; 0 = static (camera motion is added by the engine)

uniform mat4 uGBufferView;                       // set by Material::bindGBuffer (camera view, rigid)

// hasEnvReflection: the material already adds a fallback reflection (sky colour, probe...). SSR then REPLACES that
// share of the colour where its ray hits instead of adding on top of it.
void WriteGBuffer(vec3 worldNormal, float perceptualRoughness, float metallic, vec3 albedo, bool hasEnvReflection)
{
    float r = clamp(perceptualRoughness, 0.045, 1.0);
    gbNormalRoughness = vec4(normalize(mat3(uGBufferView) * worldNormal), r);
    gbMaterial        = vec4(clamp(metallic, 0.0, 1.0), hasEnvReflection ? 1.0 : 0.0, 0.0, 1.0);
    gbAlbedo          = vec4(clamp(albedo, 0.0, 1.0), 1.0);
    gbObjectVelocity  = vec2(0.0);
}
)GLSL";
}
