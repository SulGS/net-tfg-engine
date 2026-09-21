#include "ShaderLoader.hpp"

void ShaderLoader::registerBuiltIn(const std::string& key, const char* source)
{
    builtIns()[key] = source;
}

bool ShaderLoader::fetchSource(const std::string& key, std::string& code, bool& fromAsset)
{
    auto& registered = builtIns();
    auto it = registered.find(key);
    if (it != registered.end()) {
        code = it->second;
        fromAsset = false;
        return true;
    }

    auto asset = AssetManager::instance().loadAsset<ShaderSource>(key);
    if (!asset) return false;

    code = asset->code;
    fromAsset = true;
    return true;
}

// Cached create — the only path Material should use
GLuint ShaderLoader::createProgram(const std::string& vertexAssetKey,
    const std::string& fragmentAssetKey)
{
    CacheKey key{ vertexAssetKey, fragmentAssetKey };
    auto& c = cache();

    // Cache hit � just bump the ref count
    auto it = c.find(key);
    if (it != c.end()) {
        ++it->second.refCount;
        Debug::Info("ShaderLoader") << "Cache hit for shader: "
            << vertexAssetKey << " + " << fragmentAssetKey << "\n";
        return it->second.program;
    }

    // Cache miss - get the sources (engine built-ins or game assets)
    std::string vertCode, fragCode;
    bool vertFromAsset = false, fragFromAsset = false;

    if (!fetchSource(vertexAssetKey, vertCode, vertFromAsset)) {
        Debug::Error("ShaderLoader") << "Failed to load vertex shader asset: " << vertexAssetKey << "\n";
        return 0;
    }

    if (!fetchSource(fragmentAssetKey, fragCode, fragFromAsset)) {
        if (vertFromAsset) AssetManager::instance().unloadAsset<ShaderSource>(vertexAssetKey);
        Debug::Error("ShaderLoader") << "Failed to load fragment shader asset: " << fragmentAssetKey << "\n";
        return 0;
    }

    GLuint program = compileAndLink(vertCode, fragCode);

    // Sources are CPU-only text - release the asset references immediately
    if (vertFromAsset) AssetManager::instance().unloadAsset<ShaderSource>(vertexAssetKey);
    if (fragFromAsset) AssetManager::instance().unloadAsset<ShaderSource>(fragmentAssetKey);

    if (!program) return 0;

    c[key] = { program, 1 };
    Debug::Info("ShaderLoader") << "Compiled and cached shader: "
        << vertexAssetKey << " + " << fragmentAssetKey << "\n";
    return program;
}

// Cached destroy — decrements ref, deletes GL program only at zero
void ShaderLoader::destroyProgram(const std::string& vertexAssetKey,
    const std::string& fragmentAssetKey)
{
    CacheKey key{ vertexAssetKey, fragmentAssetKey };
    auto& c = cache();

    auto it = c.find(key);
    if (it == c.end()) return;

    --it->second.refCount;
    if (it->second.refCount == 0) {
        glDeleteProgram(it->second.program);
        c.erase(it);
        Debug::Info("ShaderLoader") << "Destroyed cached shader: "
            << vertexAssetKey << " + " << fragmentAssetKey << "\n";
    }
}

// Raw compile + link — no cache, no asset manager
GLuint ShaderLoader::compileAndLink(const std::string& vertexSource,
    const std::string& fragmentSource)
{
    GLuint vs = compileShader(vertexSource.c_str(), GL_VERTEX_SHADER);
    if (!vs) return 0;

    GLuint fs = compileShader(fragmentSource.c_str(), GL_FRAGMENT_SHADER);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    int success;
    char infoLog[512];
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        Debug::Error("ShaderLoader") << "Shader linking failed: " << infoLog << "\n";
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

GLuint ShaderLoader::compileShader(const char* source, GLenum type)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int success;
    char infoLog[512];
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        const char* typeName = (type == GL_VERTEX_SHADER) ? "Vertex" : "Fragment";
        Debug::Error("ShaderLoader") << typeName << " shader compilation failed: " << infoLog << "\n";
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}