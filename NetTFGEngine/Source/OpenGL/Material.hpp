#ifndef MATERIAL_HPP
#define MATERIAL_HPP

#include <string>
#include <unordered_map>
#include <variant>
#include "OpenGLIncludes.hpp"
#include "ShaderLoader.hpp"
#include "Utils/AssetManager.hpp"

// ---------------------------------------------------------------------------
// Supported uniform types
// ---------------------------------------------------------------------------
using UniformValue = std::variant<
    float,
    glm::vec2,
    glm::vec3,
    glm::vec4,
    glm::mat4,
    int
>;

// ---------------------------------------------------------------------------
// Material
//   Owns a shader program and all uniform state for it.
//   Multiple meshes can share a single Material (via shared_ptr).
// ---------------------------------------------------------------------------
class Material {
public:
    // Loads vertex and fragment shaders from the AssetManager by key,
    // then compiles and links them into a program.
    Material(const std::string& vertexShaderAsset,
        const std::string& fragmentShaderAsset);
    ~Material();

    // --- typed setters (location is looked up and cached on first call) ---
    void setFloat(const std::string& name, float value);
    void setInt(const std::string& name, int value);
    void setVec2(const std::string& name, const glm::vec2& value);
    void setVec3(const std::string& name, const glm::vec3& value);
    void setVec4(const std::string& name, const glm::vec4& value);
    void setMat4(const std::string& name, const glm::mat4& value);

    // Bind the shader and push all uniforms to the GPU.
    // Engine uniforms (model/view/projection) are passed in here so they
    // can be set every frame without the caller touching the uniform map.
    void bind(const glm::mat4& model,
        const glm::mat4& view,
        const glm::mat4& projection) const;

    GLuint getProgram() const { return shaderProgram; }

private:
    GLuint shaderProgram = 0;

    // Asset keys — stored so the destructor and clone can reference the cache
    std::string vertexAssetKey;
    std::string fragmentAssetKey;

    // Cached uniform locations for engine uniforms
    GLint modelLoc = -1;
    GLint viewLoc = -1;
    GLint projectionLoc = -1;

    // User uniforms: name -> { cached location, current value }
    struct UniformEntry {
        GLint        location;
        UniformValue value;
    };
    std::unordered_map<std::string, UniformEntry> uniforms;

    // Look up (and cache) a uniform location by name.
    // Returns -1 if the uniform doesn't exist in the shader.
    GLint getLocation(const std::string& name);

    // Push a single UniformEntry to the GPU (dispatches on variant type)
    static void uploadUniform(GLint location, const UniformValue& value);
};

#endif // MATERIAL_HPP