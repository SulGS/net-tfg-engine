#ifndef MESH_HPP
#define MESH_HPP

#include <string>
#include <cmath>
#include <algorithm>
#include <glm/glm.hpp>
#include "Material.hpp"
#include "Utils/AssetManager.hpp"

class Mesh {
public:
    // With no material the mesh uses the engine's default surface shader (DefaultShader.hpp, GGX PBR), through the ONE Material every such mesh shares: see DefaultMaterial(). Pass your own Material to customise or replace it.
    Mesh(const std::string& meshName,
        std::shared_ptr<Material> material = nullptr);

    ~Mesh();

    // The engine's default Material. Shared by every mesh built without one, so it holds no per-mesh state: to give a mesh its own uniform values, build it a Material of its own instead of setting them on this one.
    static std::shared_ptr<Material> DefaultMaterial();

    // Engine startup / shutdown of the default shader. InitDefaultMaterial() registers the shader with ShaderLoader and compiles it now, so the first mesh doesn't pay for it (and a bad shader shows up at launch, not mid-game); ReleaseDefaultMaterial() frees it. Both need the GL context current (they run on the render thread, see ClientWindow::startRenderThread); release before the context is destroyed.
    static void InitDefaultMaterial();
    static void ReleaseDefaultMaterial();

    void bindMaterial(const glm::mat4& model,
        const glm::mat4& view,
        const glm::mat4& projection) const;

    void draw() const;
    void drawGeometryOnly() const;

    // GBuffer pre-pass — binds only albedo (unit 0), normal map (unit 1) and MR map (unit 2),
    // then draws geometry. The caller must have already set uModel/uView/uProjection
    // on the gbuffer shader via glUniform before calling this.
    void drawGBuffer(GLuint gbufferShader) const;

    // Keep render() as a convenience wrapper for other use cases
    void render(const glm::mat4& model,
        const glm::mat4& view,
        const glm::mat4& projection) const;

    // Depth pre-pass — only MVP, no material/texture work
    void drawDepthOnly(const glm::mat4& mvp, GLuint depthShader) const;

    Material* getMaterial() const { return material.get(); }

    // World-space bounding sphere under `model` (radius scaled by its largest axis scale). False when the bounds are
    // unknown (no buffer, or one built without them): treat the mesh as visible from everywhere.
    bool worldBoundingSphere(const glm::mat4& model, glm::vec3& center, float& radius) const
    {
        if (!buffer || buffer->boundsRadius < 0.0f) return false;
        center = glm::vec3(model * glm::vec4(buffer->boundsCenter, 1.0f));
        const float sx = glm::dot(glm::vec3(model[0]), glm::vec3(model[0]));
        const float sy = glm::dot(glm::vec3(model[1]), glm::vec3(model[1]));
        const float sz = glm::dot(glm::vec3(model[2]), glm::vec3(model[2]));
        radius = buffer->boundsRadius * std::sqrt(std::max(sx, std::max(sy, sz)));
        return true;
    }

private:

	std::string meshName;

    std::optional<MeshBuffer> buffer;
    std::shared_ptr<Material> material;

    GLuint m_fallbackWhite;
    GLuint m_fallbackBlack;
    GLuint m_fallbackNormal;
    GLuint m_fallbackMR;
};

#endif // MESH_HPP