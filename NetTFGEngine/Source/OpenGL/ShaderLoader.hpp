#ifndef SHADER_LOADER_HPP
#define SHADER_LOADER_HPP

#include <string>
#include "OpenGLIncludes.hpp"
#include "Utils/Debug/Debug.hpp"
#include "Utils/AssetManager.hpp"

class ShaderLoader {
public:
    // Registers shader source that is compiled into the engine (see DefaultShader.hpp), as opposed to the game assets packed in the .bin files. createProgram() looks a key up here before asking the AssetManager, so a Material can name a built-in shader like any other. Keys start with "engine/" to keep them apart from game asset names. Needs no GL context; call it before the first Material that uses the key is created.
    static void registerBuiltIn(const std::string& key, const char* source);

    // Cached path: looks up or compiles a program keyed on asset paths, incrementing its ref count. This is what Material should call.
    static GLuint createProgram(const std::string& vertexAssetKey,
        const std::string& fragmentAssetKey);

    // Releases one reference. Deletes the GL program only when refcount hits 0.
    static void destroyProgram(const std::string& vertexAssetKey,
        const std::string& fragmentAssetKey);

private:
    ShaderLoader() = default; // non-instantiable

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

    static std::unordered_map<std::string, std::string>& builtIns()
    {
        static std::unordered_map<std::string, std::string> instance;
        return instance;
    }

    // Source for `key`: a built-in if one is registered, otherwise the game asset of that name (fromAsset = true, so the caller must unloadAsset it afterwards).
    static bool fetchSource(const std::string& key, std::string& code, bool& fromAsset);

    // Raw compilation — no caching, no asset manager involvement
    static GLuint compileAndLink(const std::string& vertexSource,
        const std::string& fragmentSource);

    static GLuint compileShader(const char* source, GLenum type);
};

#endif // SHADER_LOADER_HPP