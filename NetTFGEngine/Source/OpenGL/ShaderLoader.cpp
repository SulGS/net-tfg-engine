#include "ShaderLoader.hpp"

void ShaderLoader::registerBuiltIn(const std::string& key, const char* source)
{
    builtIns()[key] = source;
}

bool ShaderLoader::expandIncludes(std::string& code, const std::string& sourceName)
{
    // Depth cap only guards against an include cycle; real nesting is one or two levels.
    for (int pass = 0; pass < 8; ++pass) {
        bool expanded = false;
        size_t pos = 0;
        while ((pos = code.find("#include", pos)) != std::string::npos) {
            // Only a directive at the start of a line (indentation allowed), not "#include" inside a comment.
            size_t lineStart = code.rfind('\n', pos);
            lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
            if (code.find_first_not_of(" \t", lineStart) != pos) { pos += 8; continue; }

            size_t lineEnd = code.find('\n', pos);
            if (lineEnd == std::string::npos) lineEnd = code.size();
            const size_t q0 = code.find('"', pos);
            const size_t q1 = (q0 == std::string::npos) ? q0 : code.find('"', q0 + 1);
            if (q0 == std::string::npos || q1 == std::string::npos || q1 > lineEnd) {
                Debug::Error("ShaderLoader") << sourceName << ": malformed #include\n";
                return false;
            }

            const std::string key = code.substr(q0 + 1, q1 - q0 - 1);
            auto it = builtIns().find(key);
            if (it == builtIns().end()) {
                Debug::Error("ShaderLoader") << sourceName << ": #include \"" << key << "\" is not a registered built-in\n";
                return false;
            }
            code.replace(pos, lineEnd - pos, it->second);
            pos += it->second.size();
            expanded = true;
        }
        if (!expanded) return true;
    }
    Debug::Error("ShaderLoader") << sourceName << ": #include nested too deep (cycle?)\n";
    return false;
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
    return acquireProgram(vertexAssetKey, fragmentAssetKey, "", "");
}

GLuint ShaderLoader::createVariantProgram(const std::string& vertexAssetKey,
    const std::string& fragmentAssetKey,
    const std::string& define,
    const std::string& preamble)
{
    return acquireProgram(vertexAssetKey, fragmentAssetKey, define, preamble);
}

GLuint ShaderLoader::acquireProgram(const std::string& vertexAssetKey,
    const std::string& fragmentAssetKey,
    const std::string& define,
    const std::string& preamble)
{
    const std::string displayFrag = variantKey(fragmentAssetKey, define);
    CacheKey key{ vertexAssetKey, displayFrag };
    auto& c = cache();

    // Cache hit � just bump the ref count
    auto it = c.find(key);
    if (it != c.end()) {
        ++it->second.refCount;
        Debug::Info("ShaderLoader") << "Cache hit for shader: "
            << vertexAssetKey << " + " << displayFrag << "\n";
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

    // Sources are CPU-only text - release the asset references once compiled (or once we know there's nothing to do)
    auto releaseSources = [&]() {
        if (vertFromAsset) AssetManager::instance().unloadAsset<ShaderSource>(vertexAssetKey);
        if (fragFromAsset) AssetManager::instance().unloadAsset<ShaderSource>(fragmentAssetKey);
        };

    if (!expandIncludes(vertCode, vertexAssetKey) || !expandIncludes(fragCode, fragmentAssetKey)) {
        releaseSources();
        return 0;
    }

    if (!define.empty()) {
        // Shader doesn't implement this variant: not an error, the caller falls back to its default path.
        if (fragCode.find(define) == std::string::npos) {
            releaseSources();
            return 0;
        }

        // The preamble must follow #version (it has to be the first statement); without one, prepend.
        size_t insertAt = 0;
        const size_t versionPos = fragCode.find("#version");
        if (versionPos != std::string::npos) {
            const size_t eol = fragCode.find('\n', versionPos);
            insertAt = (eol == std::string::npos) ? fragCode.size() : eol + 1;
        }
        fragCode.insert(insertAt, preamble + "\n");
    }

    GLuint program = compileAndLink(vertCode, fragCode);
    releaseSources();

    if (!program) {
        if (!define.empty())
            Debug::Error("ShaderLoader") << "Failed to build variant " << displayFrag << "\n";
        return 0;
    }

    c[key] = { program, 1 };
    Debug::Info("ShaderLoader") << "Compiled and cached shader: "
        << vertexAssetKey << " + " << displayFrag << "\n";
    return program;
}

// Cached destroy — decrements ref, deletes GL program only at zero
void ShaderLoader::destroyProgram(const std::string& vertexAssetKey,
    const std::string& fragmentAssetKey)
{
    releaseProgram({ vertexAssetKey, fragmentAssetKey });
}

void ShaderLoader::destroyVariantProgram(const std::string& vertexAssetKey,
    const std::string& fragmentAssetKey,
    const std::string& define)
{
    releaseProgram({ vertexAssetKey, variantKey(fragmentAssetKey, define) });
}

void ShaderLoader::releaseProgram(const CacheKey& key)
{
    auto& c = cache();

    auto it = c.find(key);
    if (it == c.end()) return;

    --it->second.refCount;
    if (it->second.refCount == 0) {
        glDeleteProgram(it->second.program);
        Debug::Info("ShaderLoader") << "Destroyed cached shader: "
            << key.first << " + " << key.second << "\n";
        c.erase(it);
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