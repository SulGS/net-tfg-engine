#ifndef SHADER_LOADER_HPP
#define SHADER_LOADER_HPP

#include <string>
#include "OpenGLIncludes.hpp"
#include "Utils/Debug/Debug.hpp"
#include "Utils/AssetManager.hpp"

class ShaderLoader {
public:
    // ---------------------------------------------------------------------------
    // Cached path — looks up or compiles a program keyed on asset paths.
    // This is what Material should call. Ref count is incremented on every call.
    // ---------------------------------------------------------------------------
    static GLuint createProgram(const std::string& vertexAssetKey,
        const std::string& fragmentAssetKey);

    // Releases one reference. Deletes the GL program only when refcount hits 0.
    static void destroyProgram(const std::string& vertexAssetKey,
        const std::string& fragmentAssetKey);

private:
    ShaderLoader() = default; // non-instantiable

    // ---------------------------------------------------------------------------
    // Cache internals
    // ---------------------------------------------------------------------------
    using CacheKey = std::pair<std::string, std::string>;

    struct CacheEntry {
        GLuint  program = 0;
        uint32_t refCount = 0;
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            size_t h1 = std::hash<std::string>{}(k.first);
            size_t h2 = std::hash<std::string>{}(k.second);
            return h1 ^ (h2 << 32) ^ (h2 >> 32);
        }
    };

    static std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>& cache()
    {
        static std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> instance;
        return instance;
    }

    // ---------------------------------------------------------------------------
    // Raw compilation — no caching, no asset manager involvement
    // ---------------------------------------------------------------------------
    static GLuint compileAndLink(const std::string& vertexSource,
        const std::string& fragmentSource);

    static GLuint compileShader(const char* source, GLenum type);
};

#endif // SHADER_LOADER_HPP