#include "Material.hpp"
#include "Utils/Debug/Debug.hpp"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
Material::Material(const std::string& vertexShaderAsset,
    const std::string& fragmentShaderAsset)
    : vertexAssetKey(vertexShaderAsset)
    , fragmentAssetKey(fragmentShaderAsset)
{
    // ShaderLoader handles cache lookup, AssetManager loading, and compilation
    shaderProgram = ShaderLoader::createProgram(vertexAssetKey, fragmentAssetKey);
    if (!shaderProgram) {
        Debug::Error("Material") << "Failed to create shader program.\n";
        return;
    }

    // Cache engine uniform locations (silently -1 if the shader doesn't use them)
    modelLoc = glGetUniformLocation(shaderProgram, "uModel");
    viewLoc = glGetUniformLocation(shaderProgram, "uView");
    projectionLoc = glGetUniformLocation(shaderProgram, "uProjection");
}

Material::~Material() {
    ShaderLoader::destroyProgram(vertexAssetKey, fragmentAssetKey);
}

// ---------------------------------------------------------------------------
// Typed setters
// ---------------------------------------------------------------------------
void Material::setFloat(const std::string& name, float value) {
    GLint loc = getLocation(name);
    if (loc != -1) uniforms[name] = { loc, value };
}

void Material::setInt(const std::string& name, int value) {
    GLint loc = getLocation(name);
    if (loc != -1) uniforms[name] = { loc, value };
}

void Material::setIVec2(const std::string& name, const glm::ivec2& value) {
	GLint loc = getLocation(name);
	if (loc != -1) uniforms[name] = { loc, value };
}

void Material::setVec2(const std::string& name, const glm::vec2& value) {
    GLint loc = getLocation(name);
    if (loc != -1) uniforms[name] = { loc, value };
}

void Material::setVec3(const std::string& name, const glm::vec3& value) {
    GLint loc = getLocation(name);
    if (loc != -1) uniforms[name] = { loc, value };
}

void Material::setVec4(const std::string& name, const glm::vec4& value) {
    GLint loc = getLocation(name);
    if (loc != -1) uniforms[name] = { loc, value };
}

void Material::setMat4(const std::string& name, const glm::mat4& value) {
    GLint loc = getLocation(name);
    if (loc != -1) uniforms[name] = { loc, value };
}

// ---------------------------------------------------------------------------
// Bind — pushes all state to the GPU
// ---------------------------------------------------------------------------
void Material::bind(const glm::mat4& model,
    const glm::mat4& view,
    const glm::mat4& projection) const
{
    if (!shaderProgram) {
        Debug::Error("Material") << "Cannot bind: shader program not initialized.\n";
        return;
    }

    glUseProgram(shaderProgram);

    // Engine uniforms
    if (modelLoc != -1) glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
    if (viewLoc != -1) glUniformMatrix4fv(viewLoc, 1, GL_FALSE, glm::value_ptr(view));
    if (projectionLoc != -1) glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, glm::value_ptr(projection));

    // User uniforms
    for (const auto& [name, entry] : uniforms) {
        uploadUniform(entry.location, entry.value);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
GLint Material::getLocation(const std::string& name)
{
    // If we already cached this one, return it directly
    auto it = uniforms.find(name);
    if (it != uniforms.end()) return it->second.location;

    // First time seeing this name — look it up
    GLint loc = glGetUniformLocation(shaderProgram, name.c_str());
    if (loc == -1) {
        Debug::Warning("Material") << "Uniform '" << name << "' not found in shader.\n";
    }
    return loc;
}

void Material::uploadUniform(GLint location, const UniformValue& value)
{
    std::visit([location](const auto& v) {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, float>) {
            glUniform1f(location, v);
        }
        else if constexpr (std::is_same_v<T, int>) {
            glUniform1i(location, v);
        }
        else if constexpr (std::is_same_v<T, glm::vec2>) {
            glUniform2fv(location, 1, glm::value_ptr(v));
        }
        else if constexpr (std::is_same_v<T, glm::vec3>) {
            glUniform3fv(location, 1, glm::value_ptr(v));
        }
        else if constexpr (std::is_same_v<T, glm::vec4>) {
            glUniform4fv(location, 1, glm::value_ptr(v));
        }
        else if constexpr (std::is_same_v<T, glm::mat4>) {
            glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(v));
        }
        }, value);
}