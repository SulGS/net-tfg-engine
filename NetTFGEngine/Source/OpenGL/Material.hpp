#ifndef MATERIAL_HPP
#define MATERIAL_HPP

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include "OpenGLIncludes.hpp"
#include "ShaderLoader.hpp"
#include "Utils/AssetManager.hpp"

using UniformValue = std::variant<
    float,
    glm::vec2,
    glm::vec3,
    glm::vec4,
    glm::mat4,
    int
>;

// Owns a shader program and all uniform state for it; multiple meshes can share one Material via shared_ptr.
class Material {
public:
    Material(const std::string& vertexShaderAsset,
        const std::string& fragmentShaderAsset);
    ~Material();

    // typed setters (location is looked up and cached on first call)
    void setFloat(const std::string& name, float value);
    void setInt(const std::string& name, int value);
	void setIVec2(const std::string& name, const glm::ivec2& value);
    void setVec2(const std::string& name, const glm::vec2& value);
    void setVec3(const std::string& name, const glm::vec3& value);
    void setVec4(const std::string& name, const glm::vec4& value);
    void setMat4(const std::string& name, const glm::mat4& value);

    // For uniforms the ENGINE offers to every material (texture samplers, camera
    // position, shadow state...): a shader is free not to use them, and the GLSL
    // compiler strips any uniform it doesn't use, so a missing one is not an error.
    // The setters above warn (once per name and material) because there a missing
    // uniform is almost certainly a typo; these skip it silently instead.
    bool hasUniform(const std::string& name);
    void setIntIfPresent(const std::string& name, int value);
    void setVec3IfPresent(const std::string& name, const glm::vec3& value);

    // Bind the shader and push all uniforms to the GPU.
    // Engine uniforms (model/view/projection) are passed in here so they
    // can be set every frame without the caller touching the uniform map.
    void bind(const glm::mat4& model,
        const glm::mat4& view,
        const glm::mat4& projection) const;

    GLuint getProgram() const { return shaderProgram; }

private:
    GLuint shaderProgram = 0;

    // Asset keys � stored so the destructor and clone can reference the cache
    std::string vertexAssetKey;
    std::string fragmentAssetKey;

    GLint modelLoc = -1;
    GLint viewLoc = -1;
    GLint projectionLoc = -1;

    // User uniforms: name -> { cached location, current value }
    struct UniformEntry {
        GLint        location;
        UniformValue value;
    };
    std::unordered_map<std::string, UniformEntry> uniforms;

    // Uniform locations by name, misses (-1) included, so each name costs one
    // glGetUniformLocation for the life of the material instead of one per call.
    std::unordered_map<std::string, GLint> locationCache;
    std::unordered_set<std::string> warnedMissing;

    // Silent lookup through the cache. -1 if the uniform doesn't exist in the shader.
    GLint lookup(const std::string& name);

    // lookup() that warns, once per name, when the uniform is missing.
    GLint getLocation(const std::string& name);

    // Push a single UniformEntry to the GPU (dispatches on variant type)
    static void uploadUniform(GLint location, const UniformValue& value);
};

#endif // MATERIAL_HPP